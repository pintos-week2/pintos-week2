#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h"
#include "filesys/filesys.h"
#include "userprog/process.h"
#include "threads/palloc.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* 시스템 콜.
 *
 * 과거에는 인터럽트 핸들러가 시스템 콜을 처리했다
 * (예: Linux의 int 0x80). 그러나 x86-64에서는 제조사가
 * 시스템 콜을 효율적으로 요청할 수 있는 syscall 명령을 제공한다.
 *
 * syscall 명령은 모델별 레지스터(Model Specific Register, MSR)의
 * 값을 읽어 동작한다. 자세한 내용은 매뉴얼을 참고한다. */

#define MSR_STAR 0xc0000081         /* 세그먼트 선택자 MSR */
#define MSR_LSTAR 0xc0000082        /* 롱 모드 SYSCALL 진입 주소 */
#define MSR_SYSCALL_MASK 0xc0000084 /* eflags 마스크 */

#define is_user_vaddr(vaddr) (!is_kernel_vaddr((vaddr)))

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* syscall_entry가 사용자 스택을 커널 모드 스택으로 바꾸기 전까지는
	 * 인터럽트 서비스 루틴이 인터럽트를 처리하면 안 된다.
	 * 따라서 FLAG_FL을 마스킹한다. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}
/* 시스템 콜의 주 진입 인터페이스 */
void
syscall_handler (struct intr_frame *f) {
	// TODO: 여기에 구현을 작성한다.
	uint64_t input = f->R.rax;
	switch (input)
	{
	case SYS_HALT:
	{
		power_off();
		break;
	}
	case SYS_EXIT: //check
	{
		struct thread *t = thread_current();
		t->exit_status = f->R.rdi;
		
		thread_exit();
	}

	case SYS_FORK:
	{

		break;
	}

case SYS_EXEC: {
		/* 유저모드에서 넘긴 문자열 주소 */
		const char *cmd_line = (const char *) f->R.rdi;

		/* 잘못된 주소(NULL이거나, 커널 영역이거나, 매핑 안 된 주소)면 프로세스 종료.*/
		if (cmd_line == NULL || !is_user_vaddr(cmd_line)
	    	|| pml4_get_page(thread_current()->pml4, cmd_line) == NULL)
			exit(-1);

		/* process_exec이 현재 주소 공간을 지우면 유저 메모리에 있던
		   cmd_line도 같이 사라지므로, 커널 페이지에 미리 복사해 둔다. */
		char *cmd_copy = palloc_get_page(PAL_ZERO);
		if (cmd_copy == NULL)
			exit(-1);
		strlcpy(cmd_copy, cmd_line, PGSIZE);

		/* 성공하면 새 프로그램으로 바뀌어서 여기로 돌아오지 않는다.
		   cmd_copy는 process_exec 안에서 해제하므로 여기서 free하지 않는다. */
		if (process_exec(cmd_copy) == -1)
			exit(-1);
		NOT_REACHED();
	}

	
	case SYS_WAIT: //check
		break;
	
	case SYS_CREATE:
	{
		f->R.rax = filesys_create(f->R.rdi, f->R.rsi);
		break;
	}

	case SYS_REMOVE:
	{
		f->R.rax = filesys_remove(f->R.rdi);
		break;
	}
	
	case SYS_OPEN:
		struct file *fl = filesys_open(f->R.rdi);
		break;

	case SYS_FILESIZE:
		break;

	case SYS_READ:
		break;

	case SYS_WRITE:
		break;

	case SYS_SEEK:
		break;

	case SYS_TELL:
		break;

	case SYS_CLOSE:
		break;
	
	default:
		f->R.rax = -1;
		return;
	}

}
