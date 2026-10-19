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

/* The main system call interface */
/* f는 어셈블리가 값을 저장한 스택 영역의 주소이다.
   syscall_entry가 CPU의 저장 정보를 커널 스택에 쌓고, 
   그 스택 영역을 intr_frame 구조체로 해석해서 syscall_handler()가 사용 */
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


	case SYS_EXEC:
		break;
	
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

/* 사용자 메모리 접근을 검사하는 헬퍼 함수 */
/* 1. 사용자 주소인지 확인: 커널 영역 주소를 차단 */
/* 2. 실제로 접근 가능한지(매핑됐는지) 확인: 매핑되지 않은 주소 등의 접근을 방지 */
/* 3. 읽기, 쓰기 범위 확인: 여러 바이트에 걸친 접근에서 잘못된 영역을 놓치지 않도록 함 */
/* 4. 접근 권한 확인 */

/* 시스템 콜 함수별로 검증 함수 제작 */
/* 포인터 검사가 필요한 함수 목록 */
/* fork / exec, create, remove, open /read, write / mmap, munmap / chdir, makdir / symlink / mount, umount  */

/* 문자열 전체 검증 함수 
   1. 포인터가 NULL인지 확인한다.
   2. 현재 주소가 사용자 주소 영역인지 확인한다.
   3. 현재 주소의 페이지가 매핑되어 있는지 확인한다.
   4. 검증한 주소에서 문자를 읽는다.
   5. '\0'이면 종료하고, 아니라면 다음 문자로 이동해 반복한다.
   6. 페이지 경계를 넘으면 다음 페이지도 검사한다.*/
bool
validate_user_string(const char *ptr) {
	if (ptr == NULL)
		return false;

	struct thread *t = thread_current();
	
	while (true) {					
		if (!is_user_vaddr(ptr))		// 사용자 주소인지 확인. ptr: 문자열이 저장된 메모리 주소, *ptr: 그 주소에 저장된 문자 하나
			return false;
		
		if (!pml4_get_page(t->pml4, ptr))	// 주소가 페이지에 매핑되어있는지 확인
			return false;
		
		if (*ptr == '/0')			// 널 문자면 문자열의 끝
			return true;
	
		ptr++;						// 다음 문자로 변환
	}
}

/* 메모리 범위 검사 
   1. 포인터와 크기가 유효한지 확인한다.
   2. 버퍼가 사용하는 주소 범위가 사용자 영역인지 확인한다.
   3. 범위에 포함된 모든 페이지가 매핑되어 있는지 확인한다.
   4. 커널이 버퍼에 쓸 경우 쓰기 권한도 확인한다.
   5. 주소 계산의 오버플로도 고려한다. */
   
	/* pg_round_down(ptr);		 현재 포인터가 속한 페이지의 시작 주소
	   pg_round_down(end);		 버퍼의 끝이 속한 페이지의 시작 주소
	   pg_round_up(end);   		 버퍼의 끝이 속한 페이지의 다음 주소 */
bool
validate_user_buffer (void *buffer, size_t size)
{
    if (buffer == NULL)
        return false;

    if (size == 0)
        return true;

    uint64_t start = (uint64_t) buffer;

    // 주소 덧셈 오버플로 방지
    if (size - 1 > UINT64_MAX - start)
        return false;

    uint64_t last = start + size - 1;

    // 사용자 주소 범위인지 검사
    if (!is_user_vaddr ((void *) start) ||
        !is_user_vaddr ((void *) last))
        return false;

    uint8_t *ptr = (uint8_t *) pg_round_down (buffer);
    uint8_t *end = (uint8_t *) pg_round_down ((void *) last);

    for (; ptr <= end; ptr += PGSIZE)
    {
        if (!is_user_vaddr (ptr))
            return false;

        if (pml4_get_page (thread_current ()->pml4, ptr) == NULL)
            return false;
    }

    return true;
}

/* 단일 주소 검사
   1. 포인터가 유효한지
   2. 현재 포인터가 사용자 주소인지
   3. 주소가 잘 매핑돼있는지 */
bool
validate_user_addr(void *ptr) {
	if (ptr == NULL)
		return false;

	if (!is_user_vaddr(ptr))
		return false;

	if (!pml4_get_page(thread_current()->pml4, ptr))
		return false;
	return true;
}