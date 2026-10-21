#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "intrinsic.h"

/* 처리한 페이지 폴트 수. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* 사용자 프로그램이 일으킬 수 있는 인터럽트의 핸들러를 등록한다.

   실제 Unix 계열 운영체제에서는 [SV-386] 3-24와 3-25의 설명처럼
   이러한 인터럽트 대부분을 시그널 형태로 사용자 프로세스에 전달한다.
   하지만 여기서는 시그널을 구현하지 않으므로,
   대신 사용자 프로세스를 종료한다.

   페이지 폴트는 별도로 고려해야 한다. 지금은 다른 예외와 똑같이
   처리하지만, 가상 메모리를 구현할 때는 이 동작을 바꿔야 한다.

   각 예외에 대한 설명은 [IA32-v3a] 5.15절의
   "Exception and Interrupt Reference"를 참고한다. */
void
exception_init (void) {
	/* 사용자 프로그램은 INT, INT3, INTO, BOUND 명령 등을 통해
	   이러한 예외를 명시적으로 발생시킬 수 있다. 따라서 DPL==3으로 설정해
	   사용자 프로그램이 이 명령들로 해당 예외를 발생시킬 수 있게 한다. */
	intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
	intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
	intr_register_int (5, 3, INTR_ON, kill,
			"#BR BOUND Range Exceeded Exception");

	/* 이 예외들은 DPL==0이므로 사용자 프로세스가 INT 명령으로
	   직접 발생시킬 수 없다. 다만 간접적으로 발생할 수는 있다.
	   예를 들어 0으로 나누면 #DE가 발생할 수 있다. */
	intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
	intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
	intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
	intr_register_int (7, 0, INTR_ON, kill,
			"#NM Device Not Available Exception");
	intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
	intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
	intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
	intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
	intr_register_int (19, 0, INTR_ON, kill,
			"#XF SIMD Floating-Point Exception");

	/* 대부분의 예외는 인터럽트가 켜진 상태로 처리할 수 있다.
	   하지만 페이지 폴트 주소는 CR2에 저장되므로,
	   그 값을 보존하기 위해 페이지 폴트 진입 시 인터럽트를 꺼야 한다. */
	intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* 예외 통계를 출력한다. */
void
exception_print_stats (void) {
	printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* 사용자 프로세스가 일으킨 것으로 추정되는 예외의 핸들러. */
static void
kill (struct intr_frame *f) {
	/* 이 인터럽트는 사용자 프로세스가 일으킨 것으로 추정된다.
	   예를 들어 매핑되지 않은 가상 메모리에 접근해 페이지 폴트가
	   발생했을 수 있다. 지금은 사용자 프로세스를 종료하기만 한다.
	   나중에는 커널에서 발생한 페이지 폴트도 처리해야 한다.
	   실제 Unix 계열 운영체제는 대부분의 예외를 시그널로 프로세스에
	   전달하지만, 여기서는 시그널을 구현하지 않는다. */

	/* 인터럽트 프레임의 코드 세그먼트 값으로
	   예외가 발생한 위치를 알 수 있다. */
	switch (f->cs) {
		case SEL_UCSEG:
			/* 사용자 코드 세그먼트이므로 예상대로 사용자 예외다.
			   사용자 프로세스를 종료한다. */
			printf ("%s: dying due to interrupt %#04llx (%s).\n",
					thread_name (), f->vec_no, intr_name (f->vec_no));
			intr_dump_frame (f);
			thread_exit ();

		case SEL_KCSEG:
			/* 커널 코드 세그먼트이므로 커널 버그를 뜻한다.
			   커널 코드에서는 예외가 발생하면 안 된다. 페이지 폴트는
			   커널 예외를 일으킬 수 있지만 여기로 전달되어서는 안 된다.
			   문제를 명확히 드러내기 위해 커널 패닉을 발생시킨다. */
			intr_dump_frame (f);
			PANIC ("Kernel bug - unexpected interrupt in kernel");

		default:
			/* 다른 코드 세그먼트라면 발생해서는 안 되는 상황이다.
			   커널 패닉을 발생시킨다. */
			printf ("Interrupt %#04llx (%s) in unknown segment %04x\n",
					f->vec_no, intr_name (f->vec_no), f->cs);
			thread_exit ();
	}
}

/* 페이지 폴트 핸들러. 가상 메모리를 구현하려면 채워야 하는 뼈대 코드다.
   Project 2의 해결 방법에 따라서도 이 코드를 수정해야 할 수 있다.

   진입 시 폴트가 발생한 주소는 CR2(Control Register 2)에 들어 있다.
   폴트 정보는 exception.h의 PF_* 매크로에 설명된 형식으로
   F의 error_code 멤버에 저장된다. 아래 예제 코드는 이 정보를
   해석하는 방법을 보여준다.
   두 항목의 자세한 설명은 [IA32-v3a] 5.15절
   "Exception and Interrupt Reference"의
   "Interrupt 14--Page Fault Exception (#PF)"를 참고한다. */
static void
page_fault (struct intr_frame *f) {
	bool not_present;  /* true: 페이지가 없음, false: 읽기 전용 페이지에 쓰기. */
	bool write;        /* true: 쓰기 접근, false: 읽기 접근. */
	bool user;         /* true: 사용자 모드의 접근, false: 커널 모드의 접근. */
	void *fault_addr;  /* 폴트가 발생한 주소. */

	/* 폴트를 일으킨 가상 메모리 접근 주소를 가져온다.
	   코드나 데이터를 가리킬 수 있다. 폴트를 일으킨 명령어 자체의
	   주소와 같을 필요는 없다. 명령어 주소는 f->rip에 들어 있다. */

	fault_addr = (void *) rcr2();

	/* 인터럽트를 다시 켠다. CR2가 바뀌기 전에 안전하게 읽기 위해
	   잠시 꺼두었던 것이다. */
	intr_enable ();


	/* 원인을 판별한다. */
	not_present = (f->error_code & PF_P) == 0;
	write = (f->error_code & PF_W) != 0;
	user = (f->error_code & PF_U) != 0;

#ifdef VM
	/* Project 3 이후에 사용한다. */
	if (vm_try_handle_fault (f, fault_addr, user, write, not_present))
		return;
#endif

	/* 페이지 폴트 수를 센다. */
	page_fault_cnt++;

	/* 실제로 처리할 수 없는 폴트라면 정보를 출력하고 종료한다. */
	printf ("Page fault at %p: %s error %s page in %s context.\n",
			fault_addr,
			not_present ? "not present" : "rights violation",
			write ? "writing" : "reading",
			user ? "user" : "kernel");
	kill (f);
}

