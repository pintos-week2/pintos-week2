#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

static bool in_external_intr;
static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

/* initd와 다른 프로세스에 공통으로 사용하는 초기화 함수. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

/* FILE_NAME에서 불러온 첫 사용자 프로그램인 "initd"를 시작한다.
 * process_create_initd()가 반환하기 전에 새 스레드가 스케줄되거나
 * 종료될 수도 있다. initd의 스레드 ID를 반환하며,
 * 스레드를 생성할 수 없으면 TID_ERROR를 반환한다.
 * 이 함수는 한 번만 호출해야 한다. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);	/* 파일명 복사본을 저장할 페이지 확보 */
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);	/* 파일명을 페이지에 저장 */

	/* Create a new thread to execute FILE_NAME. */
	/* 실행할 프로세스 생성 */
	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* 첫 사용자 프로세스를 시작하는 스레드 함수. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* 현재 프로세스를 name이라는 이름으로 복제한다. 새 프로세스의
 * 스레드 ID를 반환하며, 스레드를 생성할 수 없으면 TID_ERROR를 반환한다. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	/* 현재 스레드를 새 스레드로 복제한다. */
	struct fork_status *status;
	status = malloc(sizeof(*status));
	tid_t tid;

	if (status == NULL)
		return TID_ERROR;
	
	status->parent = thread_current();
	status->parent_if = *if_;
	sema_init(&status->sema, 0);
	status->success = false;

	tid = thread_create (name, PRI_DEFAULT, __do_fork, status);
	
	if (tid == TID_ERROR) {
		free(status);
		return TID_ERROR;
	}
	
	success = status->success;

	sema_down(&status->sema);
	free(status);

	return succuess ? tid : TID_ERROR;
}

#ifndef VM
/* 이 함수를 pml4_for_each에 전달해 부모의 주소 공간을 복제한다.
 * Project 2에서만 사용한다. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	struct thread *pml4 = pml4_create ();
	bool writable;

	/* 1. TODO: parent_page가 커널 페이지라면 즉시 반환한다. */
	if (is_kern_pte(pte))
		return true;
	/* 2. 부모의 4단계 페이지 테이블에서 VA에 대응하는 페이지를 찾는다. */
	parent_page = pml4_get_page (parent->pml4, va);

	if (parent_page == NULL)
		return false;

	/* 3. TODO: 자식용 PAL_USER 페이지를 새로 할당하고,
	 *    TODO: 그 결과를 NEWPAGE에 저장한다. */
	newpage = palloc_get_page(PAL_USER);	// 물리 메모리의 페이지 하나를 할당받는 함수. 사용자 풀에서 페이지 할당.

	if (newpage == NULL)
		return false;

	/* 4. TODO: 부모의 페이지를 새 페이지에 복사하고,
	 *    TODO: 부모 페이지의 쓰기 가능 여부를 확인한다.
	 *    TODO: 확인 결과에 따라 WRITABLE을 설정한다. */
	memcpy(newpage, parent_page, PGSIZE);
	writable = (*pte & PTE_W) != 0;

	/* 5. 자식 페이지 테이블의 주소 VA에 새 페이지를 추가하고,
	 *    WRITABLE에 따라 쓰기 권한을 설정한다. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: 페이지 추가에 실패하면 오류를 처리한다. */
		palloc_free_page(newpage);
		return false;
	}
	return true;
}
#endif

/* 부모의 실행 문맥을 복사하는 스레드 함수.
 * 힌트: parent->tf에는 프로세스의 사용자 영역 실행 문맥이 없다.
 * 따라서 process_fork의 두 번째 인자를 이 함수에 전달해야 한다. */
static void
__do_fork (void *aux) {
	struct fork_status *status = aux;
	struct intr_frame if_;
	struct thread *parent = (struct thread *) status->parent;
	struct thread *current = thread_current ();
	struct semaphore *sema = &status->sema;

	/* 1. CPU 실행 문맥을 지역 스택에 복사한다. */
	if_ = status->parent_if;

	/* 2. 페이지 테이블(PT)을 복제한다. */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif
	
	/* TODO: 여기에 구현한다. file_duplicate()를 사용해 파일 객체를 복제한다. 부모는 자식이 자원을 성공적으로 복제할 때까지 fork()에서 반환하면 안 된다. */
	if (in_external_intr == false) {
		process_init ();
	}

	/* 마지막으로 새 프로세스로 전환한다. */
	status->success = true;
	if_.R.rax = 0;
	sema_up(&status->sema);
	do_iret(&if_);
	
error:
	status->success = false;
	sema_up(&status->sema);
	thread_exit ();
}

/* 현재 실행 문맥을 f_name이 지정하는 프로그램으로 교체한다.
 * 실패하면 -1을 반환한다. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* thread 구조체의 intr_frame은 사용할 수 없다.
	 * 현재 스레드가 다시 스케줄될 때 그 멤버에 실행 정보를
	 * 저장하기 때문이다. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* 먼저 현재 실행 문맥을 정리한다. */
	process_cleanup ();

	/* 그다음 바이너리를 적재한다. */
	success = load (file_name, &_if);

	/* 적재에 실패하면 종료한다. */
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* 교체된 프로그램의 실행을 시작한다. */
	do_iret (&_if);
	NOT_REACHED ();
}

/* 스레드 TID가 종료될 때까지 기다리고 종료 상태를 반환한다.
 * 예외 등으로 커널이 해당 스레드를 종료했다면 -1을 반환한다.
 * TID가 유효하지 않거나 호출 프로세스의 자식이 아니거나,
 * 같은 TID에 대해 process_wait()를 이미 성공적으로 호출했다면
 * 기다리지 않고 즉시 -1을 반환한다.
 *
 * 이 함수는 문제 2-2에서 구현한다. 현재는 아무 작업도 하지 않는다. */
int
process_wait (tid_t child_tid UNUSED) {
	/* XXX: 힌트: process_wait(initd)가 반환하면 Pintos가 종료된다.
	 * XXX:       process_wait를 구현하기 전에는 여기에
	 * XXX:       무한 루프를 넣어두는 것을 권장한다. */
	return -1;
}

/* 프로세스를 종료한다. thread_exit()에서 호출한다. */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	/* TODO: 여기에 코드를 작성한다.
	 * TODO: 프로세스 종료 메시지를 구현한다.
	 * TODO: project2/process_termination.html을 참고한다.
	 * TODO: 프로세스 자원 정리도 여기서 구현하는 것을 권장한다. */
	
	 //종료 메세지 출력
	 if (curr->pml4 != NULL)
		printf ("%s: exit(%d)\n", thread_name(), curr->exit_status);

	process_cleanup ();
}

/* 현재 프로세스의 자원을 해제한다. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* 현재 프로세스의 페이지 디렉터리를 제거하고
	 * 커널 전용 페이지 디렉터리로 전환한다. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* 여기서는 작업 순서가 매우 중요하다. 타이머 인터럽트로 인해
		 * 프로세스의 페이지 디렉터리로 다시 전환되지 않도록,
		 * 페이지 디렉터리를 바꾸기 전에 cur->pagedir를 NULL로 설정해야 한다.
		 * 또한 프로세스의 페이지 디렉터리를 제거하기 전에 기본 페이지
		 * 디렉터리를 활성화해야 한다. 그렇지 않으면 이미 해제되고
		 * 내용이 지워진 페이지 디렉터리가 활성 상태로 남게 된다. */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* 다음 스레드의 사용자 코드를 실행하도록 CPU를 설정한다.
 * 문맥 전환 때마다 호출한다. */
void
process_activate (struct thread *next) {
	/* 스레드의 페이지 테이블을 활성화한다. */
	pml4_activate (next->pml4);

	/* 인터럽트 처리에 사용할 스레드의 커널 스택을 설정한다. */
	tss_update (next);
}

/* ELF 바이너리를 적재한다. 아래 정의는 ELF 명세 [ELF1]에서
 * 거의 그대로 가져온 것이다. */

/* ELF 타입. [ELF1] 1-2를 참고한다. */
#define EI_NIDENT 16

#define PT_NULL    0            /* 무시. */
#define PT_LOAD    1            /* 적재 가능한 세그먼트. */
#define PT_DYNAMIC 2            /* 동적 링킹 정보. */
#define PT_INTERP  3            /* 동적 로더 이름. */
#define PT_NOTE    4            /* 보조 정보. */
#define PT_SHLIB   5            /* 예약됨. */
#define PT_PHDR    6            /* 프로그램 헤더 테이블. */
#define PT_STACK   0x6474e551   /* 스택 세그먼트. */

#define PF_X 1          /* 실행 가능. */
#define PF_W 2          /* 쓰기 가능. */
#define PF_R 4          /* 읽기 가능. */

/* 실행 파일 헤더. [ELF1] 1-4~1-8을 참고한다.
 * ELF 바이너리의 맨 앞에 위치한다. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* 축약 이름. */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* FILE_NAME의 ELF 실행 파일을 현재 스레드에 적재한다.
 * 실행 파일의 진입점을 *RIP에,
 * 초기 스택 포인터를 *RSP에 저장한다.
 * 성공하면 true, 실패하면 false를 반환한다. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;
	char *cmdline_copy = NULL; // 원래 명령을 복사해 둘 변수 -> 복사를 해서 원본은 보존하기 위해서 있는 복사본의 역할.
	char *argv[65];            // 나눈 단어를 담을 배열
	char *save_ptr;            // 다음 단어를 찾을 위치
	int argc = 0;              // 찾은 단어 수

	/* 페이지 디렉터리를 할당하고 활성화한다. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* strtok_r()는 문자열의 공백을 바꿔 가며 단어를 나눈다.
   	원본을 보존하고 나눌 수 있도록, 먼저 복사본을 만든다. */
	cmdline_copy = palloc_get_page(0);

	/* 페이지 할당에 실패했다면 파일 적재를 계속할 수 없으니
   	기존 정리 경로인 done으로 간다. */
	if (cmdline_copy == NULL)
    	goto done;

	/* 복사본에 원래 명령 문자열을 복사한다.
	PGSIZE는 복사본에 준비된 공간의 크기다. */
	strlcpy(cmdline_copy, file_name, PGSIZE);

	/* 첫 호출은 단어를 나눌 문자열의 시작 위치부터 읽는다.
   	반환된 첫 단어의 주소가 argv[0]에 저장된다. */
	argv[0] = strtok_r(cmdline_copy, " ", &save_ptr);

	/* 현재 칸에 단어가 있고, 허용한 인자 수 안에 있을 때만 반복한다. */
	while (argc < 64 && argv[argc] != NULL) {
    argc++;  /* 단어 수++ */
    argv[argc] = strtok_r(NULL, " ", &save_ptr);  /* 다음 칸에 다음 단어를 받는다. */
	}

	if (argc == 64 && argv[64] != NULL)                   // 64개보다 많으면
		goto done;                                       // 처리를 중단한다


	/* 인자가 하나도 없으면 실행할 프로그램 이름도 없으므로 중단한다. */
	if (argc == 0)
    	goto done;

    /* argv[0]으로 실행 파일을 연 직후, 열기에 실패했는지 확인한다. */
	file = filesys_open (argv[0]); 
	if (file == NULL) {
    	printf ("load: %s: open failed\n", argv[0]);
    	goto done;
	}

	/* 실행 파일 헤더를 읽고 검증한다. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64 아키텍처
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", argv[0]);  //인자 전달 확인
		goto done;
	}

	/* 프로그램 헤더들을 읽는다. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;
		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* 이 세그먼트는 무시한다. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* 일반적인 세그먼트.
						 * 앞부분은 디스크에서 읽고 나머지는 0으로 채운다. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* 전체를 0으로 채운다.
						 * 디스크에서는 아무것도 읽지 않는다. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* 스택을 설정한다. */
	if (!setup_stack (if_))
		goto done;
	
	uint8_t *sp = (uint8_t *) if_->rsp;

	for (i = 0; i < argc; i++) {
    	size_t len = strlen (argv[i]) + 1;

    	sp -= len;
    	memcpy (sp, argv[i], len);
    	argv[i] = (char *) sp;
	}
	sp = (uint8_t *) ((uintptr_t) sp & ~((uintptr_t) 7));

	/* argv[argc]에 해당하는 NULL 포인터를 먼저 넣는다. */
	sp -= sizeof (char *);
	*(char **) sp = NULL;

	/* argv 포인터들을 뒤에서부터 넣는다. */
	for (i = argc - 1; i >= 0; i--) {
  	  	sp -= sizeof (char *);
    	*(char **) sp = argv[i];
	}

	/* 이제 sp가 argv[0] 포인터의 주소다. */
	char **user_argv = (char **) sp;

	/* 가짜 반환 주소를 스택에 넣는다. */
	sp -= sizeof (void *);
	*(void **) sp = NULL;

	/* 사용자 프로그램에 argc와 argv를 전달한다. */
	if_->R.rdi = argc;
	if_->R.rsi = (uint64_t) user_argv;

	/* 완성된 스택 위치를 저장한다. */
	if_->rsp = (uintptr_t) sp;
		


	/* 시작 주소를 설정한다. */
	if_->rip = ehdr.e_entry;


	success = true;

done:
    file_close (file);
    if (cmdline_copy != NULL)
        palloc_free_page (cmdline_copy);
    return success;
}


/* PHDR가 유효하게 적재할 수 있는 세그먼트를 나타내는지 검사한다. FILE에서 읽을 수 있으면 true, 아니면 false를 반환한다. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset과 p_vaddr의 페이지 내 오프셋이 같아야 한다. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset이 FILE의 범위 안에 있어야 한다. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz는 p_filesz보다 크거나 같아야 한다. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* 세그먼트가 비어 있으면 안 된다. */
	if (phdr->p_memsz == 0)
		return false;

	/* 가상 메모리 영역의 시작과 끝이 모두 사용자 주소 공간 안에 있어야 한다. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* 주소가 넘쳐서 커널 가상 주소 공간을 지나
	   다시 작은 주소로 돌아가는 일이 없어야 한다. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* 0번 페이지의 매핑을 금지한다.
	   0번 페이지 매핑은 바람직하지 않을 뿐 아니라, 이를 허용하면
	   시스템 콜에 널 포인터를 전달한 사용자 코드가 memcpy() 등의
	   널 포인터 검증에 걸려 커널 패닉을 일으킬 수 있다. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* 검사를 통과했다. */
	return true;
}

#ifndef VM
/* 이 블록의 코드는 Project 2에서만 사용한다.
 * Project 2 전체에서 사용할 함수를 구현하려면
 * #ifndef 매크로 바깥에 작성한다. */

/* load()의 보조 함수. */
static bool install_page (void *upage, void *kpage, bool writable);

/* FILE의 오프셋 OFS에서 시작하는 세그먼트를 주소 UPAGE에 적재한다.
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리를
 * 다음과 같이 초기화한다.
 *
 * - UPAGE부터 READ_BYTES 바이트는
 *   FILE의 오프셋 OFS에서 읽어온다.
 *
 * - UPAGE + READ_BYTES부터 ZERO_BYTES 바이트는 0으로 채운다.
 *
 * 이 함수가 초기화한 페이지는 WRITABLE이 true이면 사용자 프로세스가
 * 쓸 수 있어야 하며, false이면 읽기 전용이어야 한다.
 *
 * 성공하면 true를 반환한다. 메모리 할당 또는 디스크 읽기에
 * 오류가 발생하면 false를 반환한다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* 이 페이지를 어떻게 채울지 계산한다. FILE에서 PAGE_READ_BYTES를 읽고 나머지 PAGE_ZERO_BYTES를 0으로 채운다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* 메모리 페이지를 가져온다. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* 이 페이지를 적재한다. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* 이 페이지를 프로세스 주소 공간에 추가한다. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* 다음 페이지로 진행한다. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* USER_STACK에 0으로 초기화한 페이지를 매핑해 최소 스택을 만든다. */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* 사용자 가상 주소 UPAGE와 커널 가상 주소 KPAGE를 페이지 테이블에 매핑한다. WRITABLE이 true면 사용자 프로세스가 쓸 수 있고 false면 읽기 전용이다. UPAGE에는 기존 매핑이 없어야 한다. KPAGE는 보통 palloc_get_page()로 사용자 풀에서 할당한 페이지다. 매핑 또는 메모리 할당에 실패하면 false, 성공하면 true를 반환한다. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* 해당 가상 주소에 기존 페이지가 없는지 확인한 뒤 새 페이지를 매핑한다. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* 여기부터는 Project 3 이후에 사용하는 코드다. Project 2에서만 함수를 구현하려면 위쪽 블록에 구현한다. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: 파일에서 세그먼트를 읽어 적재한다. */
	/* TODO: 주소 VA에서 첫 페이지 폴트가 발생할 때 호출된다. */
	/* TODO: 이 함수를 호출할 때 VA를 사용할 수 있다. */
}

/* FILE의 오프셋 OFS부터 읽은 세그먼트를 UPAGE 주소에 적재한다. 총 READ_BYTES + ZERO_BYTES 바이트를 초기화한다: UPAGE부터 READ_BYTES를 FILE의 OFS에서 읽고, 그 뒤 ZERO_BYTES를 0으로 채운다. WRITABLE이 true면 사용자 프로세스가 해당 페이지에 쓸 수 있고, false면 읽기 전용이다. 메모리 할당 또는 디스크 읽기에 실패하면 false, 성공하면 true를 반환한다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* 이 페이지를 어떻게 채울지 계산한다. FILE에서 PAGE_READ_BYTES를 읽고 나머지 PAGE_ZERO_BYTES를 0으로 채운다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* lazy_load_segment에 전달할 정보를 담을 aux를 설정한다. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* 다음 페이지로 진행한다. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* USER_STACK에 페이지를 만들고 성공 여부를 반환한다. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: stack_bottom에 스택 페이지를 매핑하고 즉시 확보한다. 성공하면 rsp를 설정하고 페이지를 스택으로 표시한다. */
	/* TODO: 여기에 구현한다. */

	return success;
}
#endif /* VM 조건부 코드의 끝. */
