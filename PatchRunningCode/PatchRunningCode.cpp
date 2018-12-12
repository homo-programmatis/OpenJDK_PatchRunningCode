#include <windows.h>

#include <conio.h>
#include <intrin.h>
#include <process.h>
#include <stdio.h>

// Force console - for those who compile this source without project
#pragma comment(linker, "/SUBSYSTEM:console /ENTRY:mainCRTStartup")

#pragma comment(lib, "Winmm.lib")	// for timeBeginPeriod()

///////////////////////////////////////////////////////////////////////////////
// Testing variables

const HANDLE g_EventStartMethod  = CreateEvent(0, FALSE, FALSE, 0);
const HANDLE g_EventMethodExited = CreateEvent(0, FALSE, FALSE, 0);
DWORD        g_NumErrors         = 0;

///////////////////////////////////////////////////////////////////////////////
// Method that will be patched while it's running

const BYTE SAMPLE_METHOD[] =
{
	0xC3,                                           // 0x00: Ret
	0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,       // 0x01: align(cache_line_size = 64), just to be extra sure to not cause unwanted effects in this cache line
	0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, // 0x08: ^
	0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, // 0x10: ^
	0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, // 0x18: ^
	0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, // 0x20: ^
	0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, // 0x28: ^
	0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, // 0x30: ^
	0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, // 0x38: ^
	0x89, 0x84, 0x24, 0x00, 0x90, 0xFF, 0xFF,       // 0x40: Patchable entry point used by OpenJDK: 'mov dword ptr [rsp-7000h],eax'
	0xE9, 0xF4, 0xFF, 0xFF, 0xFF,                   // 0x47: 'jmp' back to 0x40, making infinite loop
};

const size_t SAMPLE_METHOD_ENTRY_OFFSET  = 0x40;
const size_t SAMPLE_METHOD_TARGET_OFFSET = 0x00;
typedef void (*METHOD)();

BYTE*        g_Method            = (BYTE*)VirtualAlloc(0, sizeof(SAMPLE_METHOD), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

///////////////////////////////////////////////////////////////////////////////
// Patching code similar to code used by OpenJDK

// Similar to OpenJDK's 'AbstractICache::_flush_icache_stub'
void FlushCache(const void* a_Address)
{
	_mm_mfence();
	_mm_clflush(a_Address);
	_mm_mfence();
}

// Similar to OpenJDK's 'NativeJump::patch_verified_entry'
void PatchCodeWithJmp(BYTE* a_Where, BYTE* a_Target)
{
	const DWORD jmpOffset = (DWORD)(a_Target - (a_Where + 5));

	BYTE patch[5];
	patch[0] = 0xE9;                    // jmp opcode
	*(DWORD*)(patch + 1) = jmpOffset;   // jmp offset

	// Replace bytes [0]...[3] with a temporary endless loop,
	// to guard byte [4] from being executed while we're patching.
	*(DWORD*)a_Where = 0xFEEBFEEB;
	FlushCache(a_Where);

	// Patch byte [4]
	a_Where[4] = patch[4];
	FlushCache(a_Where);

	// Finally, patch bytes [0]...[3]
	*(DWORD*)a_Where = *(DWORD*)patch;
	FlushCache(a_Where);

	// Verify patch. The bug will already be detected here.
	if (0 != memcmp(a_Where, patch, sizeof(patch)))
	{
		g_NumErrors++;

		printf
		(
			"ERROR: Patch failed:\n"
			"  Expected = %02X%02X%02X%02X%02X\n"
			"  Actual   = %02X%02X%02X%02X%02X\n",
			patch[0], patch[1], patch[2], patch[3], patch[4],
			a_Where[0], a_Where[1], a_Where[2], a_Where[3], a_Where[4]
		);
	}
}

///////////////////////////////////////////////////////////////////////////////
// Testing environment

void* GetCrashLocation(const EXCEPTION_POINTERS* a_ExceptionInfo)
{
#ifdef _M_X64
	return (void*)a_ExceptionInfo->ContextRecord->Rip;
#else
	return (void*)a_ExceptionInfo->ContextRecord->Eip;
#endif
}

int ExceptionFilter(const EXCEPTION_POINTERS* a_ExceptionInfo)
{
	g_NumErrors++;

	printf
	(
		"ERROR: Crashed in method: CrashedAt=%p Method=%p\n",
		GetCrashLocation(a_ExceptionInfo),
		g_Method
	);

	return EXCEPTION_EXECUTE_HANDLER;
}

void ForceStackAllocation()
{
	// Allocate enough stack to prepare for 'mov dword ptr [rsp-7000h],eax'
	alloca(0x10000);
}

// Secondary thread that runs method while it's patched
void __cdecl ThreadRunMethod(void*)
{
	ForceStackAllocation();

	const METHOD method = (METHOD)(g_Method + SAMPLE_METHOD_ENTRY_OFFSET);
	for (;;)
	{
		// Wait for method body to be generated...
		WaitForSingleObject(g_EventStartMethod, INFINITE);

		// Call method. This will hang until method is patched.
		__try
		{
			method();
		}
		__except(ExceptionFilter(GetExceptionInformation()))
		{
			// Any exceptions will be processed in 'ExceptionFilter'.
		}

		// Done!
		SetEvent(g_EventMethodExited);
	}
}

// Secondary thread that hammers method's body with read operations while it's patched
void __cdecl ThreadReadMethod(void*)
{
	volatile BYTE* methodEntry = g_Method + SAMPLE_METHOD_ENTRY_OFFSET;
	const BYTE* methodEnd = g_Method + sizeof(SAMPLE_METHOD);

	static volatile DWORD s_dontOptimizeMe = 0;

	for (;;)
	{
		// Wait for method body to be generated...
		WaitForSingleObject(g_EventStartMethod, INFINITE);

		// Hammer method's entry point with read operations until it's patched.
		// Do a few extra loops after it's patched - just in case.
		for (DWORD patchedIterations = 0; patchedIterations < 1024; )
		{
			// Read method's entry point
			DWORD dontOptimizeMe = 0;
			for (volatile BYTE* pos = methodEntry; pos < methodEnd; pos++)
			{
				dontOptimizeMe += *pos;
			}

			s_dontOptimizeMe += dontOptimizeMe;

			// Is method patched already?
			// Note: On bugged CPU's any bytes could be changed, so I test them all to prevent endless test.
			if (0 != memcmp((const void*)methodEntry, SAMPLE_METHOD + SAMPLE_METHOD_ENTRY_OFFSET, 5))
				patchedIterations++;
		}

		// Done!
		SetEvent(g_EventMethodExited);
	}
}

// Secondary thread that doesn't touch the method.
void __cdecl ThreadDoNothing(void*)
{
	for (;;)
	{
		// Wait for method body to be generated...
		WaitForSingleObject(g_EventStartMethod, INFINITE);

		// Done!
		SetEvent(g_EventMethodExited);
	}
}

// The thread that patches method
void __cdecl ThreadPatchMethod(void*)
{
	// Request higher timer frequency to allow more tests per seconds
	timeBeginPeriod(1);

	DWORD lastReportTicks = GetTickCount();
	BYTE* methodEntry = g_Method + SAMPLE_METHOD_ENTRY_OFFSET;
	BYTE* patchTarget = g_Method + SAMPLE_METHOD_TARGET_OFFSET;

	for (DWORD numTests = 0; ; numTests++)
	{
		// Prepare method body.
		memcpy_s(g_Method, sizeof(SAMPLE_METHOD), SAMPLE_METHOD, sizeof(SAMPLE_METHOD));

		// Allow 'Run' thread to start.
		SetEvent(g_EventStartMethod);

		// Let 'Run' thread spin a little...
		Sleep(1);

		// Patch. This will break method's infinite loop and cause it to exit via 'ret'.
		PatchCodeWithJmp(methodEntry, patchTarget);

		// Wait for 'Run' thread...
		WaitForSingleObject(g_EventMethodExited, INFINITE);

		// Give some stats
		if (GetTickCount() > lastReportTicks + 1000)
		{
			lastReportTicks = GetTickCount();
			printf("... Tests=%d Errors=%d\n", numTests, g_NumErrors);
		}
	}
}

void RunTest()
{
	_beginthread(ThreadRunMethod, 0, 0);
	ThreadPatchMethod(0);
}

int main()
{
	printf("Select test:\n");
	printf("1. Patch while code is running\n");
	printf("2. Patch while code is being read\n");
	printf("3. Patch while code is not used\n");

	for (;;)
	{
		switch (_getch())
		{
		case '1':
			_beginthread(ThreadRunMethod, 0, 0);
			ThreadPatchMethod(0);
			return 0;
		case '2':
			_beginthread(ThreadReadMethod, 0, 0);
			ThreadPatchMethod(0);
			return 0;
		case '3':
			_beginthread(ThreadDoNothing, 0, 0);
			ThreadPatchMethod(0);
			return 0;
		}
	}

	RunTest();
	return 0;
}