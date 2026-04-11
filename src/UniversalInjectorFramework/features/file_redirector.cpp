#include "pch.h"
#include "hooks.h"
#include "injector.h"
#include "encoding.h"
#include "file_redirector.h"

#include <winternl.h>

#pragma region TypeDefs

typedef NTSTATUS(__stdcall* NtQueryDirectoryFile_t)(
	HANDLE FileHandle,
	HANDLE Event,
	PIO_APC_ROUTINE ApcRoutine,
	PVOID ApcContext,
	PIO_STATUS_BLOCK IoStatusBlock,
	PVOID FileInformation,
	ULONG Length,
	FILE_INFORMATION_CLASS FileInformationClass,
	BOOLEAN ReturnSingleEntry,
	PUNICODE_STRING FileName,
	BOOLEAN RestartScan);

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

static NtQueryDirectoryFile_t HookNtQueryDirectoryFile = nullptr;
static NtQueryDirectoryFileEx_t HookNtQueryDirectoryFileEx = nullptr;
static NtCreateFile_t HookNtCreateFile = nullptr;

#pragma endregion

#pragma region NtPathTools

static std::wstring normalize_nt_path(PUNICODE_STRING name)
{
	std::wstring path(name->Buffer, name->Length / sizeof(wchar_t));
	if (path.starts_with(L"\\??\\"))
	{
		path = path.substr(4);
	}

	return path;
}

static bool init_unicode_string(const std::wstring& path, UNICODE_STRING& out, wchar_t* buffer, size_t bufferSize)
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

static bool path_has_excluded_component(const std::filesystem::path& path)
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

static bool get_path_from_handle(HANDLE handle, std::filesystem::path& outPath)
{
	if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
	{
		return false;
	}

	wchar_t pathBuf[MAX_PATH];
	DWORD pathLength = GetFinalPathNameByHandleW(handle, pathBuf, ARRAYSIZE(pathBuf), VOLUME_NAME_DOS);

	if (pathLength == 0 || pathLength >= ARRAYSIZE(pathBuf))
	{
		return false;
	}

	std::wstring path(pathBuf, pathLength);
	if (path.starts_with(L"\\\\?\\"))
	{
		path = path.substr(4);
	}

	outPath = std::filesystem::path(path);
	return true;
}

static bool open_directory_handle(const std::filesystem::path& path, HANDLE& outHandle)
{
	outHandle = CreateFileW(
		path.c_str(),
		FILE_LIST_DIRECTORY | SYNCHRONIZE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS,
		nullptr);

	return outHandle != INVALID_HANDLE_VALUE;
}

static std::filesystem::path remove_substrings_from_path(const std::filesystem::path& path)
{
	const auto& redirector = uif::injector::instance().feature<uif::features::file_redirector>();
	if (redirector.get_removed_substrings().empty())
		return path;

	std::wstring original = path.wstring();
	std::wstring lower = original;
	std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

	for (const auto& substr : redirector.get_removed_substrings())
	{
		if (substr.empty()) continue;

		std::wstring lowerSub = substr;
		std::transform(lowerSub.begin(), lowerSub.end(), lowerSub.begin(), ::towlower);

		size_t pos = 0;
		while ((pos = lower.find(lowerSub, pos)) != std::wstring::npos)
		{
			original.erase(pos, substr.size());
			lower.erase(pos, lowerSub.size());
		}
	}

	return std::filesystem::path(original);
}

#pragma endregion

#pragma region Misc

static void load_fonts(const std::filesystem::path& patchFolderPath)
{
	for (const auto& entry : std::filesystem::directory_iterator(patchFolderPath / "dat"))
	{
		if (entry.is_regular_file() && !path_has_excluded_component(entry.path()))
		{
			if (entry.path().extension() == ".ttf" || entry.path().extension() == ".otf" || entry.path().extension() == ".ttc")
			{
				AddFontResourceExA(std::filesystem::absolute(entry.path()).string().c_str(), FR_PRIVATE, nullptr);
			}
		}
	}
}

#pragma region NtHooks

NTSTATUS __stdcall NtQueryDirectoryFileHook(
	HANDLE FileHandle,
	HANDLE Event,
	PIO_APC_ROUTINE ApcRoutine,
	PVOID ApcContext,
	PIO_STATUS_BLOCK IoStatusBlock,
	PVOID FileInformation,
	ULONG Length,
	FILE_INFORMATION_CLASS FileInformationClass,
	BOOLEAN ReturnSingleEntry,
	PUNICODE_STRING FileName,
	BOOLEAN RestartScan)
{
	auto redirectNtQueryDirectoryFile = [&](HANDLE fileHandle) -> NTSTATUS
	{
		return HookNtQueryDirectoryFile(fileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, 
			FileInformation, Length, FileInformationClass, ReturnSingleEntry, FileName, RestartScan);
	};

	if (!FileName || FileName->Length == 0)
	{
		return redirectNtQueryDirectoryFile(FileHandle);
	}
	
	std::filesystem::path fileName = normalize_nt_path(FileName);

	std::filesystem::path directoryPath;
	if (!get_path_from_handle(FileHandle, directoryPath))
	{
		return redirectNtQueryDirectoryFile(FileHandle);
	}

	auto candidatePath = (directoryPath / fileName).lexically_normal();
	candidatePath = remove_substrings_from_path(candidatePath);
	
	if (path_has_excluded_component(candidatePath))
	{
		return redirectNtQueryDirectoryFile(FileHandle);
	}

	const auto& redirector = uif::injector::instance().feature<uif::features::file_redirector>();
	auto patchPath = uif::utils::redirect_to_patch_path(candidatePath, redirector.get_patch_folder_name()).lexically_normal();

	if (patchPath != candidatePath && !path_has_excluded_component(patchPath) && std::filesystem::exists(patchPath))
	{
		HANDLE patchDirHandle = INVALID_HANDLE_VALUE;
		if (open_directory_handle(patchPath.parent_path(), patchDirHandle))
		{
			NTSTATUS status = redirectNtQueryDirectoryFile(patchDirHandle);
			CloseHandle(patchDirHandle);
			return status;
		}
	}

	return redirectNtQueryDirectoryFile(FileHandle);
}

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
	auto redirectNtQueryDirectoryFileEx = [&](HANDLE fileHandle) -> NTSTATUS
	{
		return HookNtQueryDirectoryFileEx(fileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, 
			FileInformation, Length, FileInformationClass, QueryFlags, FileName);
	};

	if (!FileName || FileName->Length == 0)
	{
		return redirectNtQueryDirectoryFileEx(FileHandle);
	}
	
	std::filesystem::path fileName = normalize_nt_path(FileName);

	std::filesystem::path directoryPath;
	if (!get_path_from_handle(FileHandle, directoryPath))
	{
		return redirectNtQueryDirectoryFileEx(FileHandle);
	}

	auto candidatePath = (directoryPath / fileName).lexically_normal();
	candidatePath = remove_substrings_from_path(candidatePath);
	
	if (path_has_excluded_component(candidatePath))
	{
		return redirectNtQueryDirectoryFileEx(FileHandle);
	}

	const auto& redirector = uif::injector::instance().feature<uif::features::file_redirector>();
	auto patchPath = uif::utils::redirect_to_patch_path(candidatePath, redirector.get_patch_folder_name()).lexically_normal();

	if (patchPath != candidatePath && !path_has_excluded_component(patchPath) && std::filesystem::exists(patchPath))
	{
		HANDLE patchDirHandle = INVALID_HANDLE_VALUE;
		if (open_directory_handle(patchPath.parent_path(), patchDirHandle))
		{
			NTSTATUS status = redirectNtQueryDirectoryFileEx(patchDirHandle);
			CloseHandle(patchDirHandle);
			return status;
		}
	}

	return redirectNtQueryDirectoryFileEx(FileHandle);
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
	std::filesystem::path candidatePath(normalizedPath);
	candidatePath = remove_substrings_from_path(candidatePath);

	if (candidatePath.is_relative() || path_has_excluded_component(candidatePath))
	{
		return redirectNtCreateFile(ObjectAttributes);
	}

	const auto& redirector = uif::injector::instance().feature<uif::features::file_redirector>();
	auto redirectedPath = uif::utils::redirect_to_patch_path(candidatePath, redirector.get_patch_folder_name()).lexically_normal();

	static std::once_flag load_fonts_flag;
	std::call_once(load_fonts_flag, [&]()
	{
		load_fonts(redirector.get_patch_folder_name());
	});
	
	if (redirectedPath.wstring() != candidatePath.wstring())
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

			if (config().contains("/file_redirector/remove_substrings_from_path"_json_pointer) && 
				config()["/file_redirector/remove_substrings_from_path"_json_pointer].is_array())
			{
				for (const auto& substr : config()["/file_redirector/remove_substrings_from_path"_json_pointer]){
					if (substr.is_string())
					{
						removed_substrings.push_back(encoding::utf8_to_utf16(substr.get<std::string>()));
					}
				}
			}

			#pragma warning(suppress: 6387)
			{
				HookNtQueryDirectoryFile = reinterpret_cast<NtQueryDirectoryFile_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryDirectoryFile"));
				HookNtQueryDirectoryFileEx = reinterpret_cast<NtQueryDirectoryFileEx_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryDirectoryFileEx"));
				HookNtCreateFile = reinterpret_cast<NtCreateFile_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
			}

			uif::hooks::hook_function(this, reinterpret_cast<void*&>(HookNtQueryDirectoryFile), reinterpret_cast<void*>(NtQueryDirectoryFileHook), "NtQueryDirectoryFile");
			uif::hooks::hook_function(this, reinterpret_cast<void*&>(HookNtQueryDirectoryFileEx), reinterpret_cast<void*>(NtQueryDirectoryFileExHook), "NtQueryDirectoryFileEx");
			uif::hooks::hook_function(this, reinterpret_cast<void*&>(HookNtCreateFile), reinterpret_cast<void*>(NtCreateFileHook), "NtCreateFile");
		}
	}
}

void uif::features::file_redirector::finalize()
{
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(HookNtQueryDirectoryFile), reinterpret_cast<void*>(NtQueryDirectoryFileHook), "NtQueryDirectoryFile");
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(HookNtQueryDirectoryFileEx), reinterpret_cast<void*>(NtQueryDirectoryFileExHook), "NtQueryDirectoryFileEx");
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(HookNtCreateFile), reinterpret_cast<void*>(NtCreateFileHook), "NtCreateFile");
}
