#include "pch.h"

#ifdef UIF_LIB_DSOUND

#include "injector.h"

struct {
	HMODULE dll;
	FARPROC DirectSoundCreate;
	FARPROC DirectSoundEnumerateA;
	FARPROC DirectSoundEnumerateW;
	FARPROC DllCanUnloadNow;
	FARPROC DllGetClassObject;
	FARPROC DirectSoundCaptureCreate;
	FARPROC DirectSoundCaptureEnumerateA;
	FARPROC DirectSoundCaptureEnumerateW;
	FARPROC GetDeviceID;
	FARPROC DirectSoundFullDuplexCreate;
	FARPROC DirectSoundCreate8;
	FARPROC DirectSoundCaptureCreate8;
} dsound;

extern "C" {
	void lib_dsound_DirectSoundCreate() { dsound.DirectSoundCreate(); }
	void lib_dsound_DirectSoundEnumerateA() { dsound.DirectSoundEnumerateA(); }
	void lib_dsound_DirectSoundEnumerateW() { dsound.DirectSoundEnumerateW(); }
	void lib_dsound_DllCanUnloadNow() { dsound.DllCanUnloadNow(); }
	void lib_dsound_DllGetClassObject() { dsound.DllGetClassObject(); }
	void lib_dsound_DirectSoundCaptureCreate() { dsound.DirectSoundCaptureCreate(); }
	void lib_dsound_DirectSoundCaptureEnumerateA() { dsound.DirectSoundCaptureEnumerateA(); }
	void lib_dsound_DirectSoundCaptureEnumerateW() { dsound.DirectSoundCaptureEnumerateW(); }
	void lib_dsound_GetDeviceID() { dsound.GetDeviceID(); }
	void lib_dsound_DirectSoundFullDuplexCreate() { dsound.DirectSoundFullDuplexCreate(); }
	void lib_dsound_DirectSoundCreate8() { dsound.DirectSoundCreate8(); }
	void lib_dsound_DirectSoundCaptureCreate8() { dsound.DirectSoundCaptureCreate8(); }
}

bool load_library_dsound() {
	dsound.dll = uif::injector::instance().load_real_library("dsound.dll");
	dsound.DirectSoundCreate = GetProcAddress(dsound.dll, "DirectSoundCreate");
	dsound.DirectSoundEnumerateA = GetProcAddress(dsound.dll, "DirectSoundEnumerateA");
	dsound.DirectSoundEnumerateW = GetProcAddress(dsound.dll, "DirectSoundEnumerateW");
	dsound.DllCanUnloadNow = GetProcAddress(dsound.dll, "DllCanUnloadNow");
	dsound.DllGetClassObject = GetProcAddress(dsound.dll, "DllGetClassObject");
	dsound.DirectSoundCaptureCreate = GetProcAddress(dsound.dll, "DirectSoundCaptureCreate");
	dsound.DirectSoundCaptureEnumerateA = GetProcAddress(dsound.dll, "DirectSoundCaptureEnumerateA");
	dsound.DirectSoundCaptureEnumerateW = GetProcAddress(dsound.dll, "DirectSoundCaptureEnumerateW");
	dsound.GetDeviceID = GetProcAddress(dsound.dll, "GetDeviceID");
	dsound.DirectSoundFullDuplexCreate = GetProcAddress(dsound.dll, "DirectSoundFullDuplexCreate");
	dsound.DirectSoundCreate8 = GetProcAddress(dsound.dll, "DirectSoundCreate8");
	dsound.DirectSoundCaptureCreate8 = GetProcAddress(dsound.dll, "DirectSoundCaptureCreate8");
	return true;
}

bool unload_library_dsound() {
	FreeLibrary(dsound.dll);
	dsound.dll = nullptr;
	return true;
}

#else

bool load_library_dsound() {
	return false;
}

bool unload_library_dsound() {
	return false;
}

#endif
