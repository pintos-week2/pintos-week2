#include "userprog/tss.h"
#include <debug.h>
#include <stddef.h>
#include "userprog/gdt.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "intrinsic.h"

/* 태스크 상태 세그먼트(Task-State Segment, TSS).
 *
 * TSS는 x86-64 전용 구조체로, 프로세서가 직접 제공하는
 * 멀티태스킹 지원 단위인 "태스크"를 정의하는 데 사용된다.
 * 하지만 이식성, 속도, 유연성 등의 이유로 대부분의 x86-64
 * 운영체제는 TSS를 거의 사용하지 않는다. Pintos도 마찬가지다.
 *
 * 그럼에도 TSS가 반드시 필요한 기능이 하나 있다.
 * 사용자 모드에서 발생한 인터럽트를 처리할 때의 스택 전환이다.
 * 사용자 모드(ring 3)에서 인터럽트가 발생하면 프로세서는 현재 TSS의
 * rsp0 멤버를 참조해 인터럽트 처리에 사용할 스택을 결정한다.
 * 따라서 TSS를 만들고 최소한 이 필드들은 초기화해야 한다.
 * 이 파일이 바로 그 작업을 수행한다.
 *
 * 인터럽트 게이트나 트랩 게이트로 인터럽트를 처리할 때
 * (여기서 처리하는 모든 인터럽트가 해당한다),
 * x86-64 프로세서는 다음과 같이 동작한다.
 *
 *   - 중단된 코드와 인터럽트 핸들러의 ring이 같으면 스택을
 *     전환하지 않는다. 커널 실행 중 발생한 인터럽트가 이 경우다.
 *     이때는 TSS의 내용과 관계없이 기존 스택을 사용한다.
 *
 *   - 중단된 코드와 핸들러의 ring이 다르면 TSS에 지정된
 *     새 ring의 스택으로 전환한다. 사용자 영역에서 발생한
 *     인터럽트가 이 경우다. 데이터 손상을 막으려면 이미 사용 중인
 *     스택으로 전환하지 않는 것이 중요하다.
 *     사용자 영역에서 실행 중일 때는 현재 프로세스의 커널 스택을
 *     사용하고 있지 않으므로, 그 스택을 항상 사용할 수 있다.
 *     따라서 스케줄러는 스레드를 전환할 때 TSS의 스택 포인터도
 *     새 스레드의 커널 스택을 가리키도록 변경한다.
 *     (호출 위치는 thread.c의 schedule이다.) */

/* 커널 TSS. */
struct task_state *tss;

/* 커널 TSS를 초기화한다. */
void
tss_init (void) {
	/* 이 TSS는 콜 게이트나 태스크 게이트에 사용하지 않는다.
	 * 따라서 실제로 참조되는 몇 개의 필드만 초기화한다. */
	tss = palloc_get_page (PAL_ASSERT | PAL_ZERO);
	tss_update (thread_current ());
}

/* 커널 TSS를 반환한다. */
struct task_state *
tss_get (void) {
	ASSERT (tss != NULL);
	return tss;
}

/* TSS의 ring 0 스택 포인터가 스레드 스택의 끝을 가리키도록 설정한다. */
void
tss_update (struct thread *next) {
	ASSERT (tss != NULL);
	tss->rsp0 = (uint64_t) next + PGSIZE;
}
