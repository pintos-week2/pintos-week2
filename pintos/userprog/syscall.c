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
#include "filesys/file.h"
#include "devices/input.h"
#include "include/lib/kernel/stdio.h"
#include "threads/synch.h"
#include "process.c"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
bool validate_user_string(const char *ptr);
bool validate_user_buffer (void *buffer, size_t size);
bool validate_user_addr(void *ptr);
struct lock file_lock;

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

	lock_init(&file_lock);
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


//파일을 열고 fd테이블과 매핑해주는 함수 구현
static int sys_open(const char *file)
{
	struct file *fl = filesys_open(file);
	struct thread *t = thread_current();
	int i = 2;

	if (fl == NULL) return -1;
	while ((i < 128) && (t->fd_list[i] != NULL)) i++;
	if (i == 128) return -1;

	t->fd_list[i] = fl;
	return i;
}   

static int sys_read(int fd, void *buffer, unsigned size)
{
	if (fd < 0 || fd == 1 || fd >= 128) return -1;
	if (validate_user_buffer(buffer, size) == false)
	{
		thread_current()->exit_status = -1;
		thread_exit();
	}
	if (size == 0) return 0;

	if (fd == 0)
	{
		uint8_t *buf = buffer;
		for (int i = 0; i < size; i++)
			buf[i] = input_getc();
		return size;
	}

	struct file *fl = thread_current()->fd_list[fd];
	if (fl == NULL) return -1;

	lock_acquire(&file_lock);
	int bytes_read = (int)file_read(fl, buffer, size);
	lock_release(&file_lock);
	
	return bytes_read;
}

static int sys_write(int fd, void *buffer, unsigned size)
{
	if (fd <= 0 || fd >= 128) return -1;
	if (validate_user_buffer(buffer, size) == false)
	{
		thread_current()->exit_status = -1;
		thread_exit();
	}
	if (size == 0) return 0;

	if (fd == 1)
	{
		char *buf = buffer;
		putbuf(buf, (size_t) size);
		return size;
	}

	struct file *fl = thread_current()->fd_list[fd];
	if (fl == NULL) return -1;

	lock_acquire(&file_lock);
	int bytes_write = (int)file_write(fl, buffer, size);
	lock_release(&file_lock);
	return bytes_write;
}

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
		lock_acquire(&file_lock);
		f->R.rax = filesys_create(f->R.rdi, f->R.rsi);
		lock_release(&file_lock);
		break;
	}

	case SYS_REMOVE:
	{
		lock_acquire(&file_lock);
		f->R.rax = filesys_remove(f->R.rdi);
		lock_release(&file_lock);
		break;
	}

	case SYS_OPEN:
	{
		lock_acquire(&file_lock);
		f->R.rax = sys_open((const char *) f->R.rdi);
		lock_release(&file_lock);
		break;
	}

	case SYS_FILESIZE:
	{
		struct file *fl = thread_current()->fd_list[(int)f->R.rdi];
		if (fl == NULL) 
		{
			f->R.rax = -1;
			break;
		}
		f->R.rax = file_length(fl);
		break;
	}

	case SYS_READ:
		f->R.rax = sys_read(f->R.rdi, (void *)f->R.rsi, f->R.rdx);
		break;

	case SYS_WRITE:
		f->R.rax = sys_write(f->R.rdi, (void *)f->R.rsi, f->R.rdx);
		break;

	case SYS_SEEK:
	{
		struct file *fl = thread_current()->fd_list[(int)f->R.rdi];
		if (fl == NULL) 
		{
			f->R.rax = -1;
			break;
		}
		file_seek(fl, (off_t)f->R.rsi);
		break;
	}

	case SYS_TELL:
	{
		struct file *fl = thread_current()->fd_list[(int)f->R.rdi];
		if (fl == NULL) 
		{
			f->R.rax = -1;
			break;
		}
		f->R.rax = file_tell(fl);
		break;
	}

	case SYS_CLOSE:
	{
		struct file *fl = thread_current()->fd_list[(int)f->R.rdi];
		if (fl == NULL) 
		{
			f->R.rax = -1;
			break;
		}
		lock_acquire(&file_lock);
		file_close(fl);
		thread_current()->fd_list[(int)f->R.rdi] = NULL;
		lock_release(&file_lock);
		break;
	}
	
	default:
		f->R.rax = -1;
		return;
	}

}
