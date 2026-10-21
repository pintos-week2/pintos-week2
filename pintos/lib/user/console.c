#include <stdio.h>
#include <string.h>
#include <syscall.h>
#include <syscall-nr.h>

/* 표준 vprintf() 함수.
   printf()와 비슷하지만 va_list를 사용한다. */
int
vprintf (const char *format, va_list args) {
	return vhprintf (STDOUT_FILENO, format, args);
}

/* printf()와 비슷하지만 지정한 HANDLE에 출력한다. */
int
hprintf (int handle, const char *format, ...) {
	va_list args;
	int retval;

	va_start (args, format);
	retval = vhprintf (handle, format, args);
	va_end (args);

	return retval;
}

/* 문자열 S를 콘솔에 출력한 뒤 줄바꿈 문자를 출력한다. */
int
puts (const char *s) {
	write (STDOUT_FILENO, s, strlen (s));
	putchar ('\n');

	return 0;
}

/* 문자 C를 콘솔에 출력한다. */
int
putchar (int c) {
	char c2 = c;
	write (STDOUT_FILENO, &c2, 1);
	return c;
}

/* vhprintf_helper()에서 사용하는 보조 데이터. */
struct vhprintf_aux {
	char buf[64];       /* 문자 버퍼. */
	char *p;            /* 버퍼 안의 현재 위치. */
	int char_cnt;       /* 지금까지 출력한 전체 문자 수. */
	int handle;         /* 출력할 파일 핸들. */
};

static void add_char (char, void *);
static void flush (struct vhprintf_aux *);

/* ARGS의 인자를 사용해 printf() 형식 문자열 FORMAT을 처리하고,
   결과를 지정한 HANDLE에 출력한다. */
int
vhprintf (int handle, const char *format, va_list args) {
	struct vhprintf_aux aux;
	aux.p = aux.buf;
	aux.char_cnt = 0;
	aux.handle = handle;
	__vprintf (format, args, add_char, &aux);
	flush (&aux);
	return aux.char_cnt;
}

/* AUX의 버퍼에 문자 C를 추가하고, 버퍼가 가득 차면 비운다. */
static void
add_char (char c, void *aux_) {
	struct vhprintf_aux *aux = aux_;
	*aux->p++ = c;
	if (aux->p >= aux->buf + sizeof aux->buf)
		flush (aux);
	aux->char_cnt++;
}

/* AUX의 버퍼에 쌓인 내용을 출력하고 버퍼를 비운다. */
static void
flush (struct vhprintf_aux *aux) {
	if (aux->p > aux->buf)
		write (aux->handle, aux->buf, aux->p - aux->buf);
	aux->p = aux->buf;
}
