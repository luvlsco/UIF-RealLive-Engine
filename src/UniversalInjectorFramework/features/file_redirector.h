#pragma once
#include "_feature_base.h"

namespace uif::features
{
	class file_redirector final : public feature_base
	{
	public:
		explicit file_redirector(uif::injector& injector) : feature_base(injector, "reallive_manager") {}

		void initialize() override;
		void finalize() override;

		const std::filesystem::path& get_patch_folder_name() const { return patch_folder_name; }
		const std::set<std::filesystem::path>& get_excluded_folders() const { return excluded_folders; }
		const std::set<std::filesystem::path>& get_excluded_extensions() const { return excluded_extensions; }
		const std::vector<std::wstring>& get_removed_substrings() const { return removed_substrings; }

	private:
		std::filesystem::path patch_folder_name;
		std::set<std::filesystem::path> excluded_folders;
		std::set<std::filesystem::path> excluded_extensions;
		std::vector<std::wstring> removed_substrings;
	};
}
