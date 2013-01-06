/*
This file is part of Darling.

Copyright (C) 2012-2013 Lubos Dolezel
Copyright (C) 2011 Shinichiro Hamaji

Darling is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Darling is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Darling.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "MachOImpl.h"
#include "log.h"
#include "RebaseState.h"
#include "BindState.h"
#include "leb.h"

#include <mach-o/loader.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#define N_WEAK_DEF			0x0080
#define FLAGS_READ_SYMTAB	1
#define FLAGS_READ_DYSYMTAB	1

template <class section>
void MachOImpl::readClassicBind(const section& sec, uint32_t* dysyms, uint32_t* symtab, const char* symstrtab)
{
	const uint32_t indirect_offset = sec.reserved1;
	const int count = sec.size / m_ptrsize;
	
	for (int i = 0; i < count; i++)
	{
		uint32_t dysym = dysyms[indirect_offset + i];
		uint32_t index = dysym & 0x3fffffff;
		nlist* sym = (nlist*)(symtab + index * (m_is64 ? 4 : 3));

		MachO::Bind* bind = new MachO::Bind();
		bind->name = symstrtab + sym->n_strx;
		bind->vmaddr = sec.addr + i * m_ptrsize;
		bind->value = sym->n_value;
		bind->type = BIND_TYPE_POINTER;
		bind->ordinal = 1;
		bind->is_weak = ((sym->n_desc & N_WEAK_DEF) != 0);
		bind->is_classic = true;

		if (!m_is64)
		{
			bind->vmaddr &= 0xffffffff;
			bind->value &= 0xffffffff;
		}

		LOG << "add classic bind: " << bind->name << " type=" << int(sym->n_type) << " sect=" << int(sym->n_sect)
			<< " desc=" << sym->n_desc << " value=" << sym->n_value << " vmaddr=" << (void*)(bind->vmaddr)
			<< " is_weak=" << bind->is_weak << std::endl;
		m_binds.push_back(bind);
    }
}

void MachOImpl::readStubBind(const section& sec,  uint32_t* dysyms, uint32_t* symtab, const char* symstrtab)
{
	const uint32_t element_size = sec.reserved2;
	const uint32_t indirect_offset = sec.reserved1;
	const int count = sec.size / element_size;

	if (element_size != 5)
	{
		LOG << "MachOImpl::readStubBind(): cannot handle stubs of elem size != 5, size=" << element_size << std::endl;
		return;
	}

	for (int i = 0; i < count; i++)
	{
		uint32_t dysym = dysyms[indirect_offset + i];
		uint32_t index = dysym & 0x3fffffff;
		nlist* sym = (nlist*)(symtab + index * 3);

		MachO::Bind* bind = new MachO::Bind();
		bind->name = symstrtab + sym->n_strx;
		bind->vmaddr = sec.addr + i * element_size;
		bind->value = sym->n_value;
		bind->type = BIND_TYPE_STUB;
		bind->ordinal = 1;
		bind->is_weak = ((sym->n_desc & N_WEAK_DEF) != 0);
		bind->is_classic = true;

		if (!m_is64)
		{
			bind->vmaddr &= 0xffffffff;
			bind->value &= 0xffffffff;
		}

		m_binds.push_back(bind);

		LOG << "add stub bind: " << bind->name.c_str() << " vmaddr=" << (void*) bind->vmaddr << std::endl;
	}
}

template <class segment_command, class section>
void MachOImpl::readSegment(char* cmds_ptr, std::vector<segment_command*>* segments, std::vector<section*>* bind_sections)
{
	segment_command* segment = reinterpret_cast<segment_command*>(cmds_ptr);
	segments->push_back(segment);

	LOG << "segment " << segment->segname << ": vmaddr=" << std::hex << segment->vmaddr
		<< " vmsize=" << segment->vmsize << " file_offset=" << segment->fileoff
		<< " file_size=" << segment->filesize << " maxprot=" << segment->maxprot
		<< " init_prot=" << segment->initprot << " nsects=" << std::dec << segment->nsects
		<< " flags=" << std::hex << segment->flags << std::dec << std::endl;

	section* sections = reinterpret_cast<section*>(cmds_ptr + sizeof(segment_command));
	
	if (!strcmp(segment->segname, "__TEXT"))
		m_text_offset = (intptr_t) segment->vmaddr; // needed for LC_MAIN
	
	for (uint32_t j = 0; j < segment->nsects; j++)
	{
		const section& sec = sections[j];
		Section savedSection{sec.segname, sec.sectname, uintptr_t(sec.addr), uintptr_t(sec.size)};
	
		LOG << "section " << sec.sectname << " in " << sec.segname << ": addr="
			<< ((void*)(uintptr_t)sec.addr) << " size=" << std::hex << sec.size << " offset="
			<< sec.offset << " align=" << sec.align << " reloff=" << sec.reloff
			<< " nreloc=" << std::dec << sec.nreloc << " flags=" << std::hex
			<< sec.flags << " reserved1=" << sec.reserved1 << " reserved2="
			<< sec.reserved2 << std::dec << std::endl;

		if (savedSection.section.size() > sizeof(sec.sectname))
			savedSection.section.resize(sizeof(sec.sectname));
		if (savedSection.segment.size() > sizeof(sec.segname))
			savedSection.segment.resize(sizeof(sec.segname));
		m_sections.push_back(savedSection);

		if (savedSection.section == "__dyld" && savedSection.segment == "__DATA")
			m_dyld_data = sec.addr;
		if (savedSection.segment == "__TEXT")
		{
			if (savedSection.section == "__eh_frame")
			{
				m_eh_frame.first = sec.addr;
				m_eh_frame.second = sec.size;
			}
			else if (savedSection.section == "__unwind_info")
			{
				m_unwind_info.first = sec.addr;
				m_unwind_info.second = sec.size;
			}
		}

		int section_type = sec.flags & SECTION_TYPE;
		switch (section_type)
		{
		case S_REGULAR: // 0x0
			/* Regular section: nothing to do */
			break;

		case S_MOD_INIT_FUNC_POINTERS: // 0x9
		{
			for (uint64_t p = sec.addr; p < sec.addr + sec.size; p += m_ptrsize)
				m_init_funcs.push_back(p);
			
			break;
		}
		case S_MOD_TERM_FUNC_POINTERS: // 0xA
			for (uint64_t p = sec.addr; p < sec.addr + sec.size; p += m_ptrsize)
				m_exit_funcs.push_back(p);
			
			break;
		case S_NON_LAZY_SYMBOL_POINTERS: // 0x6
		case S_LAZY_SYMBOL_POINTERS: // 0x7
			bind_sections->push_back(sections + j);
			break;
		
		case S_SYMBOL_STUBS: // 0x8, byte size of element in reserved2, indir offset in reserved1
			//assert(!m_is64);
			if (!m_is64)
				bind_sections->push_back(sections + j);
			break;
		case S_ZEROFILL:
		case S_CSTRING_LITERALS: // 0x2
		case S_4BYTE_LITERALS: // 0x3
		case S_8BYTE_LITERALS: // 0x4
		case S_LITERAL_POINTERS: // 0x5
		case S_COALESCED:
		case S_GB_ZEROFILL:
		case S_INTERPOSING: // 0xD
		case S_16BYTE_LITERALS:
		case S_DTRACE_DOF:
		case S_LAZY_DYLIB_SYMBOL_POINTERS:
			LOG << "Section " << sec.sectname << " in " << sec.segname << " not handled with type " << std::hex << section_type << std::dec << std::endl;
			break;

		default:
			std::cerr << "ERROR: Section " << sec.sectname << " in " << sec.segname << " has unknown type " << std::hex << section_type << std::dec << std::endl;
			abort();
			break;
		}
	}
}

void MachOImpl::readRebase(const uint8_t* p, const uint8_t* end)
{
	RebaseState state(this);
	while (p < end)
	{
		if (!state.readRebaseOp(p))
			break;
	}
}


void MachOImpl::readBind(const uint8_t* start, const uint8_t* end, bool is_weak, bool is_lazy)
{
	BindState state(this, is_weak, is_lazy);
	const uint8_t* p = start;
	while (p < end)
	{
		state.readBindOp(start, p);
	}
}

void MachOImpl::readExport(const uint8_t* start, const uint8_t* p, const uint8_t* end, std::string* name_buf)
{
	if (p >= end)
		throw std::runtime_error("broken export trie");

	if (uint8_t term_size = *p++)
	{
		const uint8_t* expected_term_end = p + term_size;
		Export* exp = new Export;
		
		exp->name = *name_buf;
		exp->flag = uleb128(p);
		
		// TODO: flag == 8 (EXPORT_SYMBOL_FLAGS_REEXPORT)
		if (exp->flag & 8)
		{
			LOG << "FIXME: reexports not currently handled\n";
			return;
		}
		
		exp->addr = uleb128(p);
		LOG << "export: " << name_buf << " flags=" << std::hex << exp->flag << std::dec << " addr=" << (void*)exp->addr << std::endl;

		m_exports.push_back(exp);

		assert(expected_term_end == p);
	}

	const uint8_t num_children = *p++;
	for (uint8_t i = 0; i < num_children; i++)
	{
		size_t orig_name_size = name_buf->size();
		while (*p)
			name_buf->push_back(*p++);
		p++;

		uint64_t off = uleb128(p);
		assert(off != 0);
		readExport(start, start + off, end, name_buf);

		name_buf->resize(orig_name_size);
	}
}

MachOImpl::MachOImpl(const char* filename, int fd, size_t offset, size_t len, bool need_exports)
	: m_mapped(0), m_mapped_size(len)
{
	m_filename = filename;
	m_need_exports = need_exports;
	m_dyld_data = 0;
	
	assert(fd > 0);

	m_fd = fd;
	m_offset = offset;
	m_text_offset = 0;
	m_main = m_entry = 0;

	if (!m_mapped_size)
		m_mapped_size = ::lseek(m_fd, 0, SEEK_END);

	::lseek(fd, 0, SEEK_SET);

	void* bin = m_mapped = reinterpret_cast<char*>(
		::mmap(NULL, m_mapped_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE, m_fd, offset)
	);
	
	if (bin == MAP_FAILED)
		throw std::runtime_error("Cannot mmap Mach-O file");
	
	m_base = uintptr_t(bin);

	const mach_header* header = reinterpret_cast<mach_header*>(bin);
	memcpy(&m_header, header, sizeof(*header));
	
	m_is64 = false;
	m_reverse_endian = false;
	if (header->magic == MH_MAGIC_64)
		m_is64 = true;
	else if (header->magic == MH_CIGAM)
	{
		m_reverse_endian = true;
		throw std::runtime_error("Unsupported endianness"); // TODO: start swapping the byte order of all structures
	}
	else if (header->magic != MH_MAGIC)
		throw std::runtime_error("Not a Mach-O file");
	
	LOG << "magic=" << std::hex << header->magic << std::dec << " cpu=" << header->cputype
		<< " cpusub=" << header->cpusubtype << " filetype=" << header->filetype << " ncmds="
		<< header->ncmds << " sizeofcmds=" << header->sizeofcmds << " flags=" << std::hex << header->flags << std::dec << std::endl;

	m_ptrsize = m_is64 ? 8 : 4;

	if ((header->cputype & 0x00ffffff) != CPU_TYPE_X86)
	{
		throw std::runtime_error("Unsupported CPU type in Mach-O");
	}

	processLoaderCommands(header);
}

void MachOImpl::processLoaderCommands(const mach_header* header)
{
	struct load_command* cmds_ptr = reinterpret_cast<struct load_command*>(
		m_base + (m_is64 ? sizeof(mach_header_64) : sizeof(mach_header))
	);

	uint32_t* symtab = 0;
	uint32_t* dysyms = 0;
	const char* symstrtab = 0;
	dyld_info_command* dyinfo = 0;

	struct relocation_info *ext_relocinfo = 0, *loc_relocinfo = 0;
	uint32_t ext_reloccount = 0, loc_reloccount = 0;

	std::vector<section_64*> bind_sections_64;
	std::vector<section*> bind_sections_32;

	for (uint32_t ii = 0; ii < header->ncmds; ii++)
	{
		LOG << "loader command type=" << std::hex << cmds_ptr->cmd << std::dec << std::endl;

		switch (cmds_ptr->cmd)
		{
		case LC_SEGMENT_64:
			readSegment<segment_command_64, section_64>((char*)cmds_ptr, &m_segments64, &bind_sections_64);
			break;
		

		case LC_SEGMENT:
			readSegment<segment_command, section>((char*)cmds_ptr, &m_segments, &bind_sections_32);
			break;

		case LC_DYLD_INFO:
		case LC_DYLD_INFO_ONLY:
		{
			dyinfo = reinterpret_cast<dyld_info_command*>(cmds_ptr);
			LOGF("dyld info: rebase_off=%u rebase_size=%u "
				"bind_off=%u bind_size=%u "
				"weak_bind_off=%u weak_bind_size=%u "
				"lazy_bind_off=%u lazy_bind_size=%u "
				"export_off=%u export_size=%u\n",
				dyinfo->rebase_off, dyinfo->rebase_size,
				dyinfo->bind_off, dyinfo->bind_size,
				dyinfo->weak_bind_off, dyinfo->weak_bind_size,
				dyinfo->lazy_bind_off, dyinfo->lazy_bind_size,
				dyinfo->export_off, dyinfo->export_size);

			{
				const uint8_t* p = reinterpret_cast<uint8_t*>(
					m_base + dyinfo->rebase_off);
				const uint8_t* end = p + dyinfo->rebase_size;
				if (dyinfo->rebase_off && dyinfo->rebase_size)
					readRebase(p, end);
			}

			{
				const uint8_t* p = reinterpret_cast<uint8_t*>(
					m_base + dyinfo->bind_off);
				const uint8_t* end = p + dyinfo->bind_size;
				readBind(p, end, false);
			}

			{
				const uint8_t* p = reinterpret_cast<uint8_t*>(
					m_base + dyinfo->lazy_bind_off);
				const uint8_t* end = p + dyinfo->lazy_bind_size;
				LOG << "Lazy bindings start at " << (void*)p << std::endl;
				readBind(p, end, false, true);
			}

			{
				const uint8_t* p = reinterpret_cast<uint8_t*>(
					m_base + dyinfo->weak_bind_off);
				const uint8_t* end = p + dyinfo->weak_bind_size;
				readBind(p, end, true);
			}

			if (m_need_exports)
			{
				const uint8_t* p = reinterpret_cast<uint8_t*>(
					m_base + dyinfo->export_off);
				const uint8_t* end = p + dyinfo->export_size;
				if (dyinfo->export_off && dyinfo->export_size)
				{
					std::string buf;
					readExport(p, p, end, &buf);
				}
			}

			break;
		}

		case LC_SYMTAB:
		{
			symtab_command* symtab_cmd = reinterpret_cast<symtab_command*>(cmds_ptr);

			LOGF("symoff=%u nsysm=%u stroff=%u strsize=%u\n",
				symtab_cmd->symoff, symtab_cmd->nsyms,
				symtab_cmd->stroff, symtab_cmd->strsize);

			uint32_t* symtab_top = symtab = reinterpret_cast<uint32_t*>(m_base + symtab_cmd->symoff);
			symstrtab = (const char*) m_base + symtab_cmd->stroff;

			if (FLAGS_READ_SYMTAB)
			{
				for (uint32_t i = 0; i < symtab_cmd->nsyms; i++)
				{
					Symbol sym;
					nlist* nl = (nlist*)symtab;
					sym.name = symstrtab + nl->n_strx;
					if (m_is64)
					{
						sym.addr = nl->n_value;
						symtab += 4;
					}
					else
					{
						sym.addr = (uint32_t)nl->n_value;
						symtab += 3;
					}

					LOGF("%d %s(%d) %p\n",
						i, sym.name.c_str(), nl->n_strx, (void*)sym.addr);
					m_symbols.push_back(sym);
				}
			}

			// Will be used by other load commands.
			symtab = symtab_top;

			break;
		}

		case LC_DYSYMTAB:
		{
			dysymtab_command* dysymtab_cmd = reinterpret_cast<dysymtab_command*>(cmds_ptr);

			LOGF("dysym:\n"
				" ilocalsym=%u nlocalsym=%u\n"
				" iextdefsym=%u nextdefsym=%u\n"
				" iundefsym=%u nundefsym=%u\n"
				" tocoff=%u ntoc=%u\n"
				" modtaboff=%u nmodtab=%u\n"
				" extrefsymoff=%u nextrefsyms=%u\n"
				" indirectsymoff=%u nindirectsyms=%u\n"
				" extreloff=%u nextrel=%u\n"
				" locreloff=%u nlocrel=%u\n"
				,
				dysymtab_cmd->ilocalsym, dysymtab_cmd->nlocalsym,
				dysymtab_cmd->iextdefsym, dysymtab_cmd->nextdefsym,
				dysymtab_cmd->iundefsym, dysymtab_cmd->nundefsym,
				dysymtab_cmd->tocoff, dysymtab_cmd->ntoc,
				dysymtab_cmd->modtaboff, dysymtab_cmd->nmodtab,
				dysymtab_cmd->extrefsymoff, dysymtab_cmd->nextrefsyms,
				dysymtab_cmd->indirectsymoff, dysymtab_cmd->nindirectsyms,
				dysymtab_cmd->extreloff, dysymtab_cmd->nextrel,
				dysymtab_cmd->locreloff, dysymtab_cmd->nlocrel);

			if (dysymtab_cmd->nindirectsyms)
			{
				dysyms = reinterpret_cast<uint32_t*>(
					m_base + dysymtab_cmd->indirectsymoff);
			}
			if (dysymtab_cmd->nextrel)
			{
				ext_reloccount = dysymtab_cmd->nextrel;
				ext_relocinfo = reinterpret_cast<relocation_info*>(m_base + dysymtab_cmd->extreloff);
			}
			if (dysymtab_cmd->nlocrel)
			{
				loc_reloccount = dysymtab_cmd->nlocrel;
				loc_relocinfo = reinterpret_cast<relocation_info*>(m_base + dysymtab_cmd->locreloff);
			}

			if (FLAGS_READ_DYSYMTAB)
			{
				for (uint32_t j = 0; j < dysymtab_cmd->nindirectsyms; j++)
				{
					uint32_t dysym = dysyms[j];
					uint32_t index = dysym & 0x3fffffff;
					const char* local =
					(dysym & INDIRECT_SYMBOL_LOCAL) ? " local" : "";
					const char* abs =
					(dysym & INDIRECT_SYMBOL_ABS) ? " abs" : "";

					uint32_t* sym = symtab;
					sym += index * (m_is64 ? 4 : 3);

					LOGF("dysym %d %s(%u)%s%s\n", j, symstrtab + sym[0], index, local, abs);
				}

				uint32_t* dymods = reinterpret_cast<uint32_t*>( m_base + dysymtab_cmd->modtaboff);
				for (uint32_t j = 0; j < dysymtab_cmd->nmodtab; j++)
					LOGF("dymods: %u\n", dymods[j]);
			}

			break;
		}

		case LC_LOAD_DYLINKER:
		{
			lc_str name = reinterpret_cast<struct dylinker_command*>(cmds_ptr)->name;
			LOG << "dynamic linker: " << ((char*)cmds_ptr + name.offset) << std::endl;
			break;
		}

		case LC_UUID:
			break;

		case LC_UNIXTHREAD:
		{
			uint32_t* p = reinterpret_cast<uint32_t*>(cmds_ptr);
			
			if (m_is64)
				m_entry = reinterpret_cast<uint64_t*>(cmds_ptr)[18];
			else
				m_entry = reinterpret_cast<uint32_t*>(cmds_ptr)[14];
			
			LOG << "UNIXTHREAD entry=" << (void*)m_entry << std::endl;
			break;
		}
		
		case LC_MAIN:
		{
			entry_point_command* cmd = reinterpret_cast<entry_point_command*>(cmds_ptr);
			LOG << "Main: entry offset: " << std::hex << cmd->entryoff << std::dec << std::endl;
			m_main = reinterpret_cast<uint64_t>(m_text_offset + cmd->entryoff);
			break;
		}

		case LC_LOAD_DYLIB:
		{
			dylib* lib = &reinterpret_cast<dylib_command*>(cmds_ptr)->dylib;
			const char* name = (char*)cmds_ptr + lib->name.offset;
			LOG << "dylib: '" << name << "'\n";
			m_dylibs.push_back(name);
			break;
		}

		case LC_RPATH:
		{
			lc_str path = reinterpret_cast<rpath_command*>(cmds_ptr)->path;
			const char* rpath = (char*)cmds_ptr + path.offset;
			LOG << "rpath: '" << rpath << "'\n";
			m_rpaths.push_back(rpath);
			break;
		}

	}

	cmds_ptr = reinterpret_cast<load_command*>(
		reinterpret_cast<char*>(cmds_ptr) + cmds_ptr->cmdsize);
	}

	//LOGF("%p vs %p\n", cmds_ptr, bin + m_mapped_size);

	LOG << "dyinfo: " << dyinfo << ", dysyms: " << dysyms << ", symtab: " << symtab << ", symstrtab: " << symstrtab << ", symbol count: " << m_symbols.size() << std::endl;
	// No LC_DYLD_INFO_ONLY, we will read classic binding info.
	if (!dyinfo && dysyms && symtab && symstrtab)
	{
		LOG << "Reading classic binding info\n";
		for (size_t i = 0; i < bind_sections_64.size(); i++)
		{
			readClassicBind<section_64>(*bind_sections_64[i], dysyms, symtab, symstrtab);
		}
		for (size_t i = 0; i < bind_sections_32.size(); i++)
		{
			if ((bind_sections_32[i]->flags & SECTION_TYPE) == S_SYMBOL_STUBS)
				readStubBind(*bind_sections_32[i], dysyms, symtab, symstrtab);
			else
				readClassicBind<section>(*bind_sections_32[i], dysyms, symtab, symstrtab);
		}
	}

	if (ext_relocinfo)
	{
		for (uint32_t i = 0; i < ext_reloccount; i++)
			readExternalRelocation(&ext_relocinfo[i], symtab, symstrtab);
	}
	if (loc_relocinfo)
	{
		for (uint32_t i = 0; i < loc_reloccount; i++)
			readInternalRelocation(&loc_relocinfo[i]);
	}
}

#if defined(__i386__)
#	define RELOC_VANILLA GENERIC_RELOC_VANILLA
#	define RELOC_PTRLEN 2
#elif defined(__x86_64__)
#	define RELOC_VANILLA X86_64_RELOC_UNSIGNED
#	define RELOC_PTRLEN 3
#endif

void MachOImpl::readInternalRelocation(const struct relocation_info* reloc)
{
	Rebase* rebase;

#ifndef __x86_64__ // "In the OS X x86-64 environment scattered relocations are not used."
	if (reloc->r_address & R_SCATTERED)
	{
		const scattered_relocation_info* scattered = reinterpret_cast<const scattered_relocation_info*>(reloc);
		if (scattered->r_type != RELOC_VANILLA)
		{
			LOG << "Unhandled internal reloc type " << scattered->r_type << std::endl;
			return;
		}

		if (scattered->r_length != RELOC_PTRLEN)
		{
			LOG << "Unsupported relocation length: " << reloc->r_length << std::endl;
			return;
		}

		rebase = new Rebase { scattered->r_address, REBASE_TYPE_POINTER };
	}
	else
#endif
	{
		if (reloc->r_symbolnum == R_ABS)
			return;
		if (reloc->r_type != RELOC_VANILLA)
		{
			LOG << "Unhandled internal reloc type " << reloc->r_type << std::endl;
			return;
		}

		if (reloc->r_length != RELOC_PTRLEN)
		{
			LOG << "Unsupported relocation length: " << reloc->r_length << std::endl;
			return;
		}

		rebase = new Rebase { uint64_t(reloc->r_address) & 0xffffffff, REBASE_TYPE_POINTER };
	}

	if (rebase)
		m_rebases.push_back(rebase);
}

void MachOImpl::readExternalRelocation(const struct relocation_info* reloc, uint32_t* symtab, const char* symstrtab)
{
	if (!reloc->r_extern)
		throw std::runtime_error("Invalid external relocation");	

	// Disabled, not implemented in original dyld
#if !defined(__x86_64__) && 0 // "In the OS X x86-64 environment scattered relocations are not used."
	if (reloc->r_address & R_SCATTERED)
	{
		const scattered_relocation_info* scattered = reinterpret_cast<const scattered_relocation_info*>(reloc);
		LOG << "Scattered relocation not handled!\n";
	}
	else
#endif
	{
		if (reloc->r_length != RELOC_PTRLEN)
		{
			LOG << "Unsupported relocation length: " << reloc->r_length << std::endl;
			return;
		}

		if (reloc->r_type == RELOC_VANILLA)
		{
			Relocation* relocation = new Relocation;
			nlist* sym = (nlist*)(symtab + reloc->r_symbolnum * (m_is64 ? 4 : 3));

			relocation->addr = reloc->r_address;
			relocation->name = symstrtab + sym->n_strx;
			relocation->pcrel = reloc->r_pcrel != 0;

			m_relocations.push_back(relocation);
		}
		else
			LOG << "Unsupported relocation type: " << reloc->r_type << std::endl;

		// At least on i386 and x86-64, this is not implemented in original dyld
		/*
		switch (reloc->r_type)
		{
#if defined(__i386__)
		case GENERIC_RELOC_VANILLA:
		case GENERIC_RELOC_PAIR:
		case GENERIC_RELOC_SECTDIFF:
		case GENERIC_RELOC_LOCAL_SECTDIFF:
		case GENERIC_RELOC_PB_LA_PTR:
#elif defined(__x86_64__)
		case X86_64_RELOC_BRANCH:
		case X86_64_RELOC_GOT_LOAD:
		case X86_64_RELOC_GOT:
		case X86_64_RELOC_SIGNED:
		case X86_64_RELOC_UNSIGNED:
		case X86_64_RELOC_SUBTRACTOR:
#elif defined (__powerpc__)
		case PPC_RELOC_VANILLA:
		case PPC_RELOC_PAIR:
		case PPC_RELOC_BR14:
		case PPC_RELOC_BR24:
		case PPC_RELOC_HI16:
		case PPC_RELOC_LO16:
		case PPC_RELOC_HA16:
		case PPC_RELOC_LO14:
		case PPC_RELOC_SECTDIFF:
		case PPC_RELOC_LOCAL_SECTDIFF:
		case PPC_RELOC_PB_LA_PTR:
		case PPC_RELOC_HI16_SECTDIFF:
		case PPC_RELOC_LO16_SECTDIFF:
		case PPC_RELOC_HA16_SECTDIFF:
		case PPC_RELOC_JBSR:
		case PPC_RELOC_LO14_SECTDIFF:
			// TODO: ppc
			LOG << "PowerPC relocations not handled!\n";
			break;
#endif
		}
		*/
	}
}

MachOImpl::~MachOImpl()
{
	close();
}

void MachOImpl::close()
{
	for (auto* b : m_binds)
		delete b;
	m_binds.clear();

	for (auto* r : m_rebases)
		delete r;
	m_rebases.clear();

	for (auto* e : m_exports)
		delete e;
	m_exports.clear();

	for (auto* r : m_relocations)
		delete r;
	m_relocations.clear();

	if (m_mapped)
	{
		::munmap(m_mapped, m_mapped_size);
		::close(m_fd);
		m_mapped = 0;
		m_fd = -1;
	}
}
