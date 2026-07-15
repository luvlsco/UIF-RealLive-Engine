#include "pch.h"
#include "resource_redirector.h"
#include "hooks.h"
#include "injector.h"
#include "utils.h"

#pragma region TypeDefs

typedef INT_PTR(WINAPI* DialogBoxParamA_t)(
	HINSTANCE hInstance,
	LPCSTR lpTemplateName,
	HWND hWndParent,
	DLGPROC lpDialogFunc,
	LPARAM dwInitParam);

typedef INT_PTR(WINAPI* DialogBoxParamW_t)(
	HINSTANCE hInstance,
	LPCWSTR lpTemplateName,
	HWND hWndParent,
	DLGPROC lpDialogFunc,
	LPARAM dwInitParam);

typedef HWND(WINAPI* CreateDialogParamA_t)(
	HINSTANCE hInstance,
	LPCSTR lpTemplateName,
	HWND hWndParent,
	DLGPROC lpDialogFunc,
	LPARAM dwInitParam);

typedef HWND(WINAPI* CreateDialogParamW_t)(
	HINSTANCE hInstance,
	LPCWSTR lpTemplateName,
	HWND hWndParent,
	DLGPROC lpDialogFunc,
	LPARAM dwInitParam);

typedef HMENU(WINAPI* LoadMenuA_t)(
	HINSTANCE hInstance,
	LPCSTR lpMenuName);

typedef HMENU(WINAPI* LoadMenuW_t)(
	HINSTANCE hInstance,
	LPCWSTR lpMenuName);

static DialogBoxParamA_t OriginalDialogBoxParamA = DialogBoxParamA;
static DialogBoxParamW_t OriginalDialogBoxParamW = DialogBoxParamW;
static CreateDialogParamA_t OriginalCreateDialogParamA = CreateDialogParamA;
static CreateDialogParamW_t OriginalCreateDialogParamW = CreateDialogParamW;
static LoadMenuA_t OriginalLoadMenuA = LoadMenuA;
static LoadMenuW_t OriginalLoadMenuW = LoadMenuW;

#pragma endregion

#pragma region Helpers

// uif::utils::get_dll_path already resolves self-DLL, wrapped with GetModuleHandleW to avoid repetitive code
static HMODULE get_self_module()
{
	return GetModuleHandleW(uif::utils::get_dll_path().c_str());
}

// Two overloads since ANSI names can be integer IDs (IS_INTRESOURCE)
static HINSTANCE resolve_dialog_source_a(HINSTANCE hInstance, LPCSTR lpTemplateName)
{
	HMODULE hSelf = get_self_module();
	if (FindResourceA(hSelf, lpTemplateName, MAKEINTRESOURCEA(5)) != NULL)
	{
		return hSelf;
	}

	return hInstance;
}

static HINSTANCE resolve_dialog_source_w(HINSTANCE hInstance, LPCWSTR lpTemplateName)
{
	HMODULE hSelf = get_self_module();
	if (FindResourceW(hSelf, lpTemplateName, RT_DIALOG) != NULL)
	{
		return hSelf;
	}

	return hInstance;
}

static HINSTANCE resolve_menu_source_a(HINSTANCE hInstance, LPCSTR lpMenuName)
{
	HMODULE hSelf = get_self_module();
	if (FindResourceA(hSelf, lpMenuName, MAKEINTRESOURCEA(4)) != NULL)
	{
		return hSelf;
	}

	return hInstance;
}

static HINSTANCE resolve_menu_source_w(HINSTANCE hInstance, LPCWSTR lpMenuName)
{
	HMODULE hSelf = get_self_module();
	if (FindResourceW(hSelf, lpMenuName, RT_MENU) != NULL)
	{
		return hSelf;
	}

	return hInstance;
}

#pragma endregion

#pragma region Hooks

INT_PTR WINAPI DialogBoxParamAHook(
	HINSTANCE hInstance,
	LPCSTR lpTemplateName,
	HWND hWndParent,
	DLGPROC lpDialogFunc,
	LPARAM dwInitParam)
{
	return OriginalDialogBoxParamA(
		resolve_dialog_source_a(hInstance, lpTemplateName),
		lpTemplateName,
		hWndParent,
		lpDialogFunc,
		dwInitParam);
}

INT_PTR WINAPI DialogBoxParamWHook(
	HINSTANCE hInstance,
	LPCWSTR lpTemplateName,
	HWND hWndParent,
	DLGPROC lpDialogFunc,
	LPARAM dwInitParam)
{
	return OriginalDialogBoxParamW(
		resolve_dialog_source_w(hInstance, lpTemplateName),
		lpTemplateName,
		hWndParent,
		lpDialogFunc,
		dwInitParam);
}

HWND WINAPI CreateDialogParamAHook(
	HINSTANCE hInstance,
	LPCSTR lpTemplateName,
	HWND hWndParent,
	DLGPROC lpDialogFunc,
	LPARAM dwInitParam)
{
	return OriginalCreateDialogParamA(
		resolve_dialog_source_a(hInstance, lpTemplateName),
		lpTemplateName,
		hWndParent,
		lpDialogFunc,
		dwInitParam);
}

HWND WINAPI CreateDialogParamWHook(
	HINSTANCE hInstance,
	LPCWSTR lpTemplateName,
	HWND hWndParent,
	DLGPROC lpDialogFunc,
	LPARAM dwInitParam)
{
	return OriginalCreateDialogParamW(
		resolve_dialog_source_w(hInstance, lpTemplateName),
		lpTemplateName,
		hWndParent,
		lpDialogFunc,
		dwInitParam);
}

HMENU WINAPI LoadMenuAHook(
	HINSTANCE hInstance,
	LPCSTR lpMenuName)
{
	return OriginalLoadMenuA(
		resolve_menu_source_a(hInstance, lpMenuName),
		lpMenuName);
}

HMENU WINAPI LoadMenuWHook(
	HINSTANCE hInstance,
	LPCWSTR lpMenuName)
{
	return OriginalLoadMenuW(
		resolve_menu_source_w(hInstance, lpMenuName),
		lpMenuName);
}

#pragma endregion

void uif::features::resource_redirector::initialize()
{
	if (!config().value("/enable"_json_pointer, false))
		return;

	if (config().value("/resource_redirector/enable"_json_pointer, false))
	{
		uif::hooks::hook_function(this, reinterpret_cast<void*&>(OriginalDialogBoxParamA), reinterpret_cast<void*>(DialogBoxParamAHook), "DialogBoxParamA");
		uif::hooks::hook_function(this, reinterpret_cast<void*&>(OriginalDialogBoxParamW), reinterpret_cast<void*>(DialogBoxParamWHook), "DialogBoxParamW");
		uif::hooks::hook_function(this, reinterpret_cast<void*&>(OriginalCreateDialogParamA), reinterpret_cast<void*>(CreateDialogParamAHook), "CreateDialogParamA");
		uif::hooks::hook_function(this, reinterpret_cast<void*&>(OriginalCreateDialogParamW), reinterpret_cast<void*>(CreateDialogParamWHook), "CreateDialogParamW");
		uif::hooks::hook_function(this, reinterpret_cast<void*&>(OriginalLoadMenuA), reinterpret_cast<void*>(LoadMenuAHook), "LoadMenuA");
		uif::hooks::hook_function(this, reinterpret_cast<void*&>(OriginalLoadMenuW), reinterpret_cast<void*>(LoadMenuWHook), "LoadMenuW");
	}
}

void uif::features::resource_redirector::finalize()
{
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(OriginalDialogBoxParamA), reinterpret_cast<void*>(DialogBoxParamAHook), "DialogBoxParamA");
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(OriginalDialogBoxParamW), reinterpret_cast<void*>(DialogBoxParamWHook), "DialogBoxParamW");
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(OriginalCreateDialogParamA), reinterpret_cast<void*>(CreateDialogParamAHook), "CreateDialogParamA");
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(OriginalCreateDialogParamW), reinterpret_cast<void*>(CreateDialogParamWHook), "CreateDialogParamW");
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(OriginalLoadMenuA), reinterpret_cast<void*>(LoadMenuAHook), "LoadMenuA");
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(OriginalLoadMenuW), reinterpret_cast<void*>(LoadMenuWHook), "LoadMenuW");
}
