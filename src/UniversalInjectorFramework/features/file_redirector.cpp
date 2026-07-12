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

typedef NTSTATUS(__stdcall* NtOpenFile_t)(
	PHANDLE FileHandle,
	ACCESS_MASK DesiredAccess,
	POBJECT_ATTRIBUTES ObjectAttributes,
	PIO_STATUS_BLOCK IoStatusBlock,
	ULONG ShareAccess,
	ULONG OpenOptions);

typedef NTSTATUS(__stdcall* NtQueryFullAttributesFile_t)(
	POBJECT_ATTRIBUTES ObjectAttributes,
	PVOID FileInformation);

typedef NTSTATUS(__stdcall* NtQueryAttributesFile_t)(
	POBJECT_ATTRIBUTES ObjectAttributes,
	PVOID FileInformation);

static NtQueryDirectoryFile_t HookNtQueryDirectoryFile = nullptr;
static NtQueryDirectoryFileEx_t HookNtQueryDirectoryFileEx = nullptr;
static NtCreateFile_t HookNtCreateFile = nullptr;
static NtOpenFile_t HookNtOpenFile = nullptr;
static NtQueryFullAttributesFile_t HookNtQueryFullAttributesFile = nullptr;
static NtQueryAttributesFile_t HookNtQueryAttributesFile = nullptr;

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

static std::filesystem::path try_redirect_to_patch_directory(
	const std::filesystem::path& originalDir,
	const std::wstring& searchPattern)
{
	const auto& redirector = uif::injector::instance().feature<uif::features::file_redirector>();
	auto patchDir = uif::utils::redirect_to_patch_path(originalDir, redirector.get_patch_folder_name());

	if (patchDir == originalDir)
	{
		return originalDir;
	}

	auto fullPath = patchDir / searchPattern;
	WIN32_FIND_DATAW fd;
	HANDLE hFind = FindFirstFileW(fullPath.c_str(), &fd);
	if (hFind != INVALID_HANDLE_VALUE)
	{
		FindClose(hFind);
		return patchDir;
	}

	return originalDir;
}

#pragma endregion

#pragma region Misc

constexpr size_t BUFFER_SIZE_SMALL = 1024;
constexpr size_t BUFFER_SIZE_LARGE = 32768;

struct PathContext
{
	std::wstring_view originalPathView;
	std::filesystem::path filteredPath;
};

static PathContext extract_path_context(POBJECT_ATTRIBUTES objAttrs)
{
	std::wstring originalPath = normalize_nt_path(objAttrs->ObjectName);
	std::wstring_view originalView(objAttrs->ObjectName->Buffer, objAttrs->ObjectName->Length / sizeof(wchar_t));
	std::filesystem::path filtered = remove_substrings_from_path(std::filesystem::path(originalPath));
	return {originalView, filtered};
}

template<typename CallFn>
static NTSTATUS call_with_modified_path(
	const std::optional<std::wstring>& modifiedPath,
	POBJECT_ATTRIBUTES objAttrs,
	CallFn&& redirect,
	size_t bufferSize = BUFFER_SIZE_LARGE)
{
	if (!modifiedPath)
	{
		return redirect(objAttrs);
	}

	static thread_local wchar_t buffer[BUFFER_SIZE_LARGE];
	UNICODE_STRING newName;
	if (!init_unicode_string(*modifiedPath, newName, buffer, bufferSize))
	{
		return redirect(objAttrs);
	}

	OBJECT_ATTRIBUTES modifiedAttrs = *objAttrs;
	modifiedAttrs.ObjectName = &newName;
	return redirect(&modifiedAttrs);
}

static std::wstring apply_nt_prefix(const std::wstring_view& origView, const std::wstring& path)
{
	std::wstring result = path;
	if (origView.starts_with(L"\\??\\") && !result.starts_with(L"\\??\\"))
	{
		result = L"\\??\\" + result;
	}
	return result;
}

static std::optional<std::wstring> get_modified_path_if_changed(
	const std::wstring_view& origView,
	const std::filesystem::path& modified,
	POBJECT_ATTRIBUTES objAttrs)
{
	if (modified.wstring() == normalize_nt_path(objAttrs->ObjectName))
	{
		return std::nullopt;
	}
	return apply_nt_prefix(origView, modified.wstring());
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
	auto redirect = [&](HANDLE fileHandle) -> NTSTATUS {
		return HookNtQueryDirectoryFile(fileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
			FileInformation, Length, FileInformationClass, ReturnSingleEntry, FileName, RestartScan);
	};

	if (!FileName || FileName->Length == 0)
		return redirect(FileHandle);

	std::filesystem::path searchPattern = normalize_nt_path(FileName);
	std::filesystem::path directoryPath;
	if (!get_path_from_handle(FileHandle, directoryPath))
		return redirect(FileHandle);

	auto filteredDir = remove_substrings_from_path(directoryPath);
	auto excludedCheckPath = filteredDir / searchPattern;

	if (path_has_excluded_component(excludedCheckPath))
		return redirect(FileHandle);

	std::filesystem::path searchPatternPath(searchPattern);
	auto filteredPattern = remove_substrings_from_path(searchPatternPath).wstring();
	if (filteredPattern != searchPattern.wstring())
	{
		static thread_local wchar_t buffer[BUFFER_SIZE_SMALL];
		UNICODE_STRING newName;
		if (init_unicode_string(filteredPattern, newName, buffer, BUFFER_SIZE_SMALL))
		{
			return HookNtQueryDirectoryFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
				FileInformation, Length, FileInformationClass, ReturnSingleEntry, &newName, RestartScan);
		}
	}

	auto effectiveDir = try_redirect_to_patch_directory(filteredDir, searchPattern);
	if (effectiveDir != directoryPath)
	{
		HANDLE patchHandle;
		if (open_directory_handle(effectiveDir, patchHandle))
		{
			NTSTATUS status = redirect(patchHandle);
			CloseHandle(patchHandle);
			return status;
		}
	}

	return redirect(FileHandle);
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
	auto redirect = [&](HANDLE fileHandle) -> NTSTATUS {
		return HookNtQueryDirectoryFileEx(fileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
			FileInformation, Length, FileInformationClass, QueryFlags, FileName);
	};

	if (!FileName || FileName->Length == 0)
		return redirect(FileHandle);

	std::filesystem::path searchPattern = normalize_nt_path(FileName);
	std::filesystem::path directoryPath;
	if (!get_path_from_handle(FileHandle, directoryPath))
		return redirect(FileHandle);

	auto filteredDir = remove_substrings_from_path(directoryPath);
	auto excludedCheckPath = filteredDir / searchPattern;

	if (path_has_excluded_component(excludedCheckPath))
		return redirect(FileHandle);

	std::filesystem::path searchPatternPath(searchPattern);
	auto filteredPattern = remove_substrings_from_path(searchPatternPath).wstring();
	if (filteredPattern != searchPattern.wstring())
	{
		static thread_local wchar_t buffer[BUFFER_SIZE_SMALL];
		UNICODE_STRING newName;
		if (init_unicode_string(filteredPattern, newName, buffer, BUFFER_SIZE_SMALL))
		{
			return HookNtQueryDirectoryFileEx(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
				FileInformation, Length, FileInformationClass, QueryFlags, &newName);
		}
	}

	auto effectiveDir = try_redirect_to_patch_directory(filteredDir, searchPattern);
	if (effectiveDir != directoryPath)
	{
		HANDLE patchHandle;
		if (open_directory_handle(effectiveDir, patchHandle))
		{
			NTSTATUS status = redirect(patchHandle);
			CloseHandle(patchHandle);
			return status;
		}
	}

	return redirect(FileHandle);
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
	auto redirect = [&](POBJECT_ATTRIBUTES objAttrs) -> NTSTATUS {
		return HookNtCreateFile(FileHandle, DesiredAccess, objAttrs, IoStatusBlock,
			AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
	};

	if (!ObjectAttributes || !ObjectAttributes->ObjectName)
	{
		return redirect(ObjectAttributes);
	}

	auto [originalPathView, filteredPath] = extract_path_context(ObjectAttributes);

	if (filteredPath.is_relative() || path_has_excluded_component(filteredPath))
	{
		return redirect(ObjectAttributes);
	}

	const auto& redirector = uif::injector::instance().feature<uif::features::file_redirector>();
	auto patchPath = uif::utils::redirect_to_patch_path(filteredPath, redirector.get_patch_folder_name()).lexically_normal();

	if (patchPath.wstring() != filteredPath.wstring())
	{
		auto finalPath = get_modified_path_if_changed(originalPathView, patchPath, ObjectAttributes);
		if (auto status = call_with_modified_path(finalPath, ObjectAttributes, redirect); NT_SUCCESS(status))
		{
			return status;
		}
	}

	auto finalPath = get_modified_path_if_changed(originalPathView, filteredPath, ObjectAttributes);
	return call_with_modified_path(finalPath, ObjectAttributes, redirect);
}

NTSTATUS __stdcall NtOpenFileHook(
	PHANDLE FileHandle,
	ACCESS_MASK DesiredAccess,
	POBJECT_ATTRIBUTES ObjectAttributes,
	PIO_STATUS_BLOCK IoStatusBlock,
	ULONG ShareAccess,
	ULONG OpenOptions)
{
	auto redirect = [&](POBJECT_ATTRIBUTES objAttrs) -> NTSTATUS {
		return HookNtOpenFile(FileHandle, DesiredAccess, objAttrs, IoStatusBlock, ShareAccess, OpenOptions);
	};

	if (!ObjectAttributes || !ObjectAttributes->ObjectName)
	{
		return redirect(ObjectAttributes);
	}

	auto [originalPathView, filteredPath] = extract_path_context(ObjectAttributes);

	if (filteredPath.is_relative() || path_has_excluded_component(filteredPath))
	{
		return redirect(ObjectAttributes);
	}

	auto finalPath = get_modified_path_if_changed(originalPathView, filteredPath, ObjectAttributes);
	return call_with_modified_path(finalPath, ObjectAttributes, redirect);
}

NTSTATUS __stdcall NtQueryFullAttributesFileHook(
	POBJECT_ATTRIBUTES ObjectAttributes,
	PVOID FileInformation)
{
	auto redirect = [&](POBJECT_ATTRIBUTES objAttrs) -> NTSTATUS {
		return HookNtQueryFullAttributesFile(objAttrs, FileInformation);
	};

	if (!ObjectAttributes || !ObjectAttributes->ObjectName)
	{
		return redirect(ObjectAttributes);
	}

	auto [originalPathView, filteredPath] = extract_path_context(ObjectAttributes);

	if (filteredPath.is_relative() || path_has_excluded_component(filteredPath))
	{
		return redirect(ObjectAttributes);
	}

	auto finalPath = get_modified_path_if_changed(originalPathView, filteredPath, ObjectAttributes);
	return call_with_modified_path(finalPath, ObjectAttributes, redirect);
}

NTSTATUS __stdcall NtQueryAttributesFileHook(
	POBJECT_ATTRIBUTES ObjectAttributes,
	PVOID FileInformation)
{
	auto redirect = [&](POBJECT_ATTRIBUTES objAttrs) -> NTSTATUS {
		return HookNtQueryAttributesFile(objAttrs, FileInformation);
	};

	if (!ObjectAttributes || !ObjectAttributes->ObjectName)
	{
		return redirect(ObjectAttributes);
	}

	auto [originalPathView, filteredPath] = extract_path_context(ObjectAttributes);

	if (filteredPath.is_relative() || path_has_excluded_component(filteredPath))
	{
		return redirect(ObjectAttributes);
	}

	auto finalPath = get_modified_path_if_changed(originalPathView, filteredPath, ObjectAttributes);
	return call_with_modified_path(finalPath, ObjectAttributes, redirect);
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
				HookNtOpenFile = reinterpret_cast<NtOpenFile_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtOpenFile"));
				HookNtQueryFullAttributesFile = reinterpret_cast<NtQueryFullAttributesFile_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryFullAttributesFile"));
				HookNtQueryAttributesFile = reinterpret_cast<NtQueryAttributesFile_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryAttributesFile"));
			}

			uif::hooks::hook_function(this, reinterpret_cast<void*&>(HookNtQueryDirectoryFile), reinterpret_cast<void*>(NtQueryDirectoryFileHook), "NtQueryDirectoryFile");
			uif::hooks::hook_function(this, reinterpret_cast<void*&>(HookNtQueryDirectoryFileEx), reinterpret_cast<void*>(NtQueryDirectoryFileExHook), "NtQueryDirectoryFileEx");
			uif::hooks::hook_function(this, reinterpret_cast<void*&>(HookNtCreateFile), reinterpret_cast<void*>(NtCreateFileHook), "NtCreateFile");
			uif::hooks::hook_function(this, reinterpret_cast<void*&>(HookNtOpenFile), reinterpret_cast<void*>(NtOpenFileHook), "NtOpenFile");
			uif::hooks::hook_function(this, reinterpret_cast<void*&>(HookNtQueryFullAttributesFile), reinterpret_cast<void*>(NtQueryFullAttributesFileHook), "NtQueryFullAttributesFile");
			uif::hooks::hook_function(this, reinterpret_cast<void*&>(HookNtQueryAttributesFile), reinterpret_cast<void*>(NtQueryAttributesFileHook), "NtQueryAttributesFile");
		}
	}
}

void uif::features::file_redirector::finalize()
{
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(HookNtQueryDirectoryFile), reinterpret_cast<void*>(NtQueryDirectoryFileHook), "NtQueryDirectoryFile");
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(HookNtQueryDirectoryFileEx), reinterpret_cast<void*>(NtQueryDirectoryFileExHook), "NtQueryDirectoryFileEx");
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(HookNtCreateFile), reinterpret_cast<void*>(NtCreateFileHook), "NtCreateFile");
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(HookNtOpenFile), reinterpret_cast<void*>(NtOpenFileHook), "NtOpenFile");
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(HookNtQueryFullAttributesFile), reinterpret_cast<void*>(NtQueryFullAttributesFileHook), "NtQueryFullAttributesFile");
	uif::hooks::unhook_function(this, reinterpret_cast<void*&>(HookNtQueryAttributesFile), reinterpret_cast<void*>(NtQueryAttributesFileHook), "NtQueryAttributesFile");
}
