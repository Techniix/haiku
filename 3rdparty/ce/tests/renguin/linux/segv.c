/* A Linux program that faults: a write to address 0x10 with no SIGSEGV
 * handler. It must die by SIGSEGV (exit status 139 in a shell), and an
 * attached debugger must be told of the fault (renguin_test
 * brand.debugger_fault, Chromium R-8). */
#include "sys.h"

int check_main(int argc, char** argv)
{
	(void)argc; (void)argv;
	*(volatile int*)0x10 = 1;
	return 1;
}
