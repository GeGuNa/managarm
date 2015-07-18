
#include "../../frigg/include/types.hpp"
#include "util/general.hpp"
#include "runtime.hpp"
#include "debug.hpp"
#include "../../frigg/include/elf.hpp"
#include "util/vector.hpp"
#include "util/smart-ptr.hpp"
#include "memory/physical-alloc.hpp"
#include "memory/paging.hpp"
#include "memory/kernel-alloc.hpp"
#include "core.hpp"
#include "schedule.hpp"
#include "../../hel/include/hel.h"

using namespace thor;

LazyInitializer<debug::VgaScreen> vgaScreen;
LazyInitializer<debug::Terminal> vgaTerminal;
LazyInitializer<debug::TerminalLogger> vgaLogger;

LazyInitializer<memory::StupidPhysicalAllocator> stupidTableAllocator;

void *loadInitImage(memory::PageSpace *space, uintptr_t image_page) {
	char *image = (char *)memory::physicalToVirtual(image_page);

	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)image;
	if(ehdr->e_ident[0] != '\x7F'
			|| ehdr->e_ident[1] != 'E'
			|| ehdr->e_ident[2] != 'L'
			|| ehdr->e_ident[3] != 'F') {
		vgaLogger->log("Illegal magic fields");
		debug::panic();
	}
	if(ehdr->e_type != ET_EXEC) {
		vgaLogger->log("init image must be ET_EXEC");
		debug::panic();
	}
	
	for(int i = 0; i < ehdr->e_phnum; i++) {
		Elf64_Phdr *phdr = (Elf64_Phdr*)(image + ehdr->e_phoff
				+ i * ehdr->e_phentsize);

		uintptr_t bottom = phdr->p_vaddr;
		uintptr_t top = phdr->p_vaddr + phdr->p_memsz;

		if(bottom == top)
			continue;
		
		size_t page_size = 0x1000;
		uintptr_t bottom_page = bottom / page_size;
		uintptr_t top_page = top / page_size;
		uintptr_t num_pages = top_page - bottom_page;
		if(top % page_size != 0)
			num_pages++;

		auto memory = makeShared<Memory>(kernelAlloc.get());
		memory->resize(num_pages * page_size);

		for(uintptr_t page = 0; page < num_pages; page++) {
			PhysicalAddr physical = memory->getPage(page);
			for(int p = 0; p < page_size; p++)
				*((char *)memory::physicalToVirtual(physical) + p) = 0;
		}

		for(size_t p = 0; p < phdr->p_filesz; p++) {
			uintptr_t page = (phdr->p_vaddr + p) / page_size - bottom_page;
			uintptr_t virt_offset = (phdr->p_vaddr + p) % page_size;
			
			PhysicalAddr physical = memory->getPage(page);
			char *ptr = (char *)memory::physicalToVirtual(physical);
			*(ptr + virt_offset) = *(image + phdr->p_offset + p);
		}

		for(uintptr_t page = 0; page < num_pages; page++) {
			PhysicalAddr physical = memory->getPage(page);
			
			space->mapSingle4k((void *)((bottom_page + page) * page_size),
					physical);
		}
	}
	
	return (void *)ehdr->e_entry;
}

extern "C" void thorMain(uint64_t init_image) {
	vgaScreen.initialize((char *)memory::physicalToVirtual(0xB8000), 80, 25);
	
	vgaTerminal.initialize(vgaScreen.get());
	vgaTerminal->clear();

	vgaLogger.initialize(vgaTerminal.get());
	vgaLogger->log("Starting Thor");
	debug::criticalLogger = vgaLogger.get();

	stupidTableAllocator.initialize(0x800000);
	memory::tableAllocator = stupidTableAllocator.get();
	
	thorRtInitializeProcessor();
	
	memory::kernelSpace.initialize(0x301000);
	kernelAlloc.initialize();
	
	irqRelays.initialize();
	thorRtSetupIrqs();

	memory::PageSpace user_space = memory::kernelSpace->clone();
	user_space.switchTo();
	
	auto entry = (void (*)(uintptr_t))loadInitImage(&user_space, init_image);
	thorRtInvalidateSpace();

	auto universe = makeShared<Universe>(kernelAlloc.get());
	auto address_space = makeShared<AddressSpace>(kernelAlloc.get(), user_space);

	auto thread = makeShared<Thread>(kernelAlloc.get());
	thread->setup(entry, 0, nullptr);
	thread->setUniverse(universe->shared<Universe>());
	thread->setAddressSpace(address_space->shared<AddressSpace>());
	
	currentThread.initialize(SharedPtr<Thread>());
	scheduleQueue.initialize();

	scheduleQueue->addBack(util::move(thread));
	schedule();
}

extern "C" void thorDoubleFault() {
	vgaLogger->log("Double fault");
	debug::panic();
}

extern "C" void thorPageFault(uintptr_t address, Word error) {
	vgaLogger->log("Page fault");
	vgaLogger->log((void *)address);
	vgaLogger->log((void *)thorRtUserContext->rip);
	vgaLogger->log(error);

	uint8_t *disasm = (uint8_t *)thorRtUserContext->rip;
	for(size_t i = 0; i < 5; i++)
		vgaLogger->logHex(disasm[i]);

	debug::panic();
}

extern "C" void thorIrq(int irq) {
	thorRtAcknowledgeIrq(irq);

	(*irqRelays)[irq].fire();

	if(irq == 0) {
		schedule();
	}else{
		thorRtFullReturn();
	}
	vgaLogger->log("Other irq");
	
	vgaLogger->log("No return at end of thorIrq()");
	debug::panic();
}

extern "C" void thorSyscall(Word index, Word arg0, Word arg1,
		Word arg2, Word arg3, Word arg4, Word arg5) {
	switch(index) {
		case kHelCallLog: {
			HelError error = helLog((const char *)arg0, (size_t)arg1);

			thorRtReturnSyscall1((Word)error);
		}
		case kHelCallPanic: {
			HelError error = helLog((const char *)arg0, (size_t)arg1);
			
			while(true) { }
		}

		case kHelCallAllocateMemory: {
			HelHandle handle;
			HelError error = helAllocateMemory((size_t)arg0, &handle);
			
			thorRtReturnSyscall2((Word)error, (Word)handle);
		}
		case kHelCallMapMemory: {
			HelError error = helMapMemory((HelHandle)arg0,
					(void *)arg1, (size_t)arg2);

			thorRtReturnSyscall1((Word)error);
		}

		case kHelCallCreateThread: {
			HelHandle handle;
			HelError error = helCreateThread((void (*) (uintptr_t))arg0,
					(uintptr_t)arg1, (void *)arg2, &handle);

			thorRtReturnSyscall2((Word)error, (Word)handle);
		}
		case kHelCallExitThisThread: {
			HelError error = helExitThisThread();
			
			schedule();
		}

		case kHelCallCreateEventHub: {
			HelHandle handle;
			HelError error = helCreateEventHub(&handle);

			thorRtReturnSyscall2((Word)error, (Word)handle);
		}
		case kHelCallWaitForEvents: {
			size_t num_items;
			HelError error = helWaitForEvents((HelHandle)arg0,
					(HelEvent *)arg1, (size_t)arg2, (HelNanotime)arg3,
					&num_items);

			thorRtReturnSyscall2((Word)error, (Word)num_items);
		}

		case kHelCallCreateBiDirectionPipe: {
			HelHandle first;
			HelHandle second;
			HelError error = helCreateBiDirectionPipe(&first, &second);
			
			thorRtReturnSyscall3((Word)error, (Word)first, (Word)second);
		}
		case kHelCallRecvString: {
			HelError error = helRecvString((HelHandle)arg0, (char *)arg1, (size_t)arg2);

			thorRtReturnSyscall1((Word)error);
		}
		case kHelCallSendString: {
			HelError error = helSendString((HelHandle)arg0, (const char *)arg1, (size_t)arg2);

			thorRtReturnSyscall1((Word)error);
		}

		case kHelCallAccessIrq: {
			HelHandle handle;
			HelError error = helAccessIrq((int)arg0, &handle);

			thorRtReturnSyscall2((Word)error, (Word)handle);
		}
		case kHelCallSubmitIrq: {
			HelError error = helSubmitIrq((HelHandle)arg0,
					(HelHandle)arg1, (int64_t)arg2,
					(uintptr_t)arg3, (uintptr_t)arg4);

			thorRtReturnSyscall1((Word)error);
		}

		case kHelCallAccessIo: {
			HelHandle handle;
			HelError error = helAccessIo((uintptr_t *)arg0, (size_t)arg1, &handle);
			
			thorRtReturnSyscall2((Word)error, (Word)handle);
		}
		case kHelCallEnableIo: {
			HelError error = helEnableIo((HelHandle)arg0);
			
			thorRtReturnSyscall1((Word)error);
		}
		default:
			vgaLogger->log("Illegal syscall");
			debug::panic();
	}
	vgaLogger->log("No return at end of thorSyscall()");
	debug::panic();
}

