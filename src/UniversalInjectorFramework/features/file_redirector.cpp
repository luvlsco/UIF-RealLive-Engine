#include "pch.h"
#include "hooks.h"
#include "injector.h"
#include "file_redirector.h"

#include <winternl.h>

#pragma region TypeDefs

typedef NTSTATUS(__stdcall* NtQueryDirectoryFileEx_t)(
	HANDLE FileHandle,
	HANDLE Event,
	PIO_APC_ROUTINE ApcRoutine,
	PVOID ApcContext,
	PIO_STATUS_BLOCK IoStatusBlock,
	PVOID FileInformation,
	ULONG Length,
	FILE_INFORMATION_CLASS FileInformationClass,
	ULONG QueryFlags,
	PUNICODE_STRING FileName);

typedef NTSTATUS(__stdcall* NtCreateFile_t)(
	PHANDLE FileHandle,
	ACCESS_MASK DesiredAccess,
	POBJECT_ATTRIBUTES ObjectAttributes,
	PIO_STATUS_BLOCK IoStatusBlock,
	PLARGE_INTEGER AllocationSize,
	ULONG FileAttributes,
	ULONG ShareAccess,
	ULONG CreateDisposition,
	ULONG CreateOptions,
	PVOID EaBuffer,
	ULONG EaLength);

static NtQueryDirectoryFileEx_t HookNtQueryDirectoryFileEx = nullptr;
static NtCreateFile_t HookNtCreateFile = nullptr;

#pragma endregion

#pragma region NtPathTools

std::wstring normalize_nt_path(PUNICODE_STRING name)
{
	std::wstring path(name->Buffer, name->Length / sizeof(wchar_t));
	if (path.starts_with(L"\\??\\"))
	{
		path = path.substr(4);
	}

	return path;
}

bool init_unicode_string(const std::wstring& path, UNICODE_STRING& out, wchar_t* buffer, size_t bufferSize)
{
	if (path.size() + 1 > bufferSize)
	{
		return false;
	}

	wcscpy_s(buffer, bufferSize, path.c_str());

	out.Buffer = buffer;
	out.Length = static_cast<USHORT>(path.size() * sizeof(wchar_t));
	out.MaximumLength = static_cast<USHORT>(bufferSize * sizeof(wchar_t));

	return true;
}

#pragma endregion

#pragma region PathFiltering

bool path_has_excluded_component(const std::filesystem::path& path)
{
	const auto& redirector = uif::injector::instance().feature<uif::features::file_redirector>();

	for (const auto& component : path)
	{
		if (redirector.get_excluded_folders().contains(uif::utils::normalize_path(component)))
		{
			return true;
		}
	}

	if(redirector.get_excluded_extensions().contains(uif::utils::normalize_path(path.extension())))
	{
		return true;
	}

	return false;
}

#pragma endregion

#pragma region NtHooks

NTSTATUS __stdcall NtQueryDirectoryFileExHook(
    HANDLE FileHandle,
    HANDLE Event,
    PIO_APC_ROUTINE ApcRoutine,
    PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation,
    ULONG Length,
    FILE_INFORMATION_CLASS FileInformationClass,
    ULONG QueryFlags,
    PUNICODE_STRING FileName)
{
	return HookNtQueryDirectoryFileEx(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation, Length, FileInformationClass, QueryFlags, FileName);
}

NTSTATUS __stdcall NtCreateFileHook(
	PHANDLE FileHandle,
	ACCESS_MASK DesiredAccess,
	POBJECT_ATTRIBUTES ObjectAttributes,
	PIO_STATUS_BLOCK IoStatusBlock,
	PLARGE_INTEGER AllocationSize,
	ULONG FileAttributes,
	ULONG ShareAccess,
	ULONG CreateDisposition,
	ULONG CreateOptions,
	PVOID EaBuffer,
	ULONG EaLength)
{
	auto redirectNtCreateFile = [&](POBJECT_ATTRIBUTES objectAttributes) -> NTSTATUS
	{
		return HookNtCreateFile(FileHandle, DesiredAccess, objectAttributes, IoStatusBlock, 
			AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
	};

	if (!ObjectAttributes || !ObjectAttributes->ObjectName)
	{
		return redirectNtCreateFile(ObjectAttributes);
	}

	std::wstring_view origPathUniView(ObjectAttributes->ObjectName->Buffer, ObjectAttributes->ObjectName->Length / sizeof(wchar_t));

	std::wstring normalizedPath = normalize_nt_path(ObjectAttributes->ObjectName);
	std::filesystem::path originalPath(normalizedPath);
	
	if (originalPath.is_relative() || path_has_excluded_component(originalPath))
	{
		return redirectNtCreateFile(ObjectAttributes);
	}

	const auto& patchFolderName = uif::injector::instance().feature<uif::features::file_redirector>();
	auto redirectedPath = uif::utils::redirect_to_patch_path(originalPath, patchFolderName.get_patch_folder_name()).lexically_normal();

	if (redirectedPath.wstring() != originalPath.wstring())
	{
		static thread_local wchar_t buffer[32768];
		std::wstring finalPath = redirectedPath.wstring();

		if (origPathUniView.starts_with(L"\\??\\") && !finalPath.starts_with(L"\\??\\"))
		{
			finalPath = L"\\??\\" + finalPath;
		}

		UNICODE_STRING newName;
		if (init_unicode_string(finalPath, newName, buffer, 32768))
		{
			OBJECT_ATTRIBUTES patchObjAttrs = *ObjectAttributes;
			patchObjAttrs.ObjectName = &newName;

			NTSTATUS redirectStatus = redirectNtCreateFile(&patchObjAttrs);
			if (NT_SUCCESS(redirectStatus))
			{
				return redirectStatus;
			}
		}
	}
	return redirectNtCreateFile(ObjectAttributes);
}

#pragma endregion

void uif::features::file_redirector::initialize()
{
	if(config().value("/enable"_json_pointer, false))
	{
		patch_folder_name = config().value("/patch_folder_name"_json_pointer, "");

		if(config().value("/file_redirector/enable"_json_pointer, false))
		{
			if (config().contains("/file_redirector/excluded_folders"_json_pointer) && 
				config()["/file_redirector/excluded_folders"_json_pointer].is_array())
			{
				for (const auto& folder : config()["/file_redirector/excluded_folders"_json_pointer])
				{
					if (folder.is_string())
					{
						excluded_folders.insert(uif::utils::normalize_path(folder.get<std::string>()));
					}
				}
			}
			
			if (config().contains("/file_redirector/excluded_extensions"_json_pointer) && 
				config()["/file_redirector/excluded_extensions"_json_pointer].is_array())
			{
				for (const auto& extension : config()["/file_redirector/excluded_extensions"_json_pointer])
				{
					if (extension.is_string())
					{
						excluded_extensions.insert(uif::utils::normalize_path(extension.get<std::string>()));
					}
				}
			}

			#pragma warning(suppress: 6387)
			HookNtQueryDirectoryFileEx = reinterpret_cast<NtQueryDirectoryFileEx_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryDirectoryFileEx"));
			HookNtCreateFile = reinterpret_cast<NtCreateFile_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));

			uif::hooks::hook_function(this, reinterpret_cast<void*&>(HookNtQueryDirectoryFileEx), reinterpret_cast<void*>(NtQueryDirectoryFileExHook), "NtQueryDirectoryFileEx");
			uif::hooks::hook_function(this, reinterpret_cast<void*&>(HookNtCreateFile), reinterpret_cast<void*>(NtCreateFileHook), "NtCreateFile");
		}
	}
}

void uif::features::file_redirector::finalize()
{
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(HookNtQueryDirectoryFileEx), reinterpret_cast<void*>(NtQueryDirectoryFileExHook), "NtQueryDirectoryFileEx");
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(HookNtCreateFile), reinterpret_cast<void*>(NtCreateFileHook), "NtCreateFile");
}
