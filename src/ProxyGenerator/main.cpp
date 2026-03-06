#define WIN32_LEAN_AND_MEAN
#include "windows.h"
#include "../Detours/detours.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>

HMODULE load_library(const char* name)
{
	const auto module = LoadLibraryExA(name, nullptr, DONT_RESOLVE_DLL_REFERENCES);
	return module;
}

struct export_symbol
{
	const unsigned long ordinal;
	const std::string name;
	const std::string safe_name;
	const bool has_name;
};

std::string get_safe_name(std::string name)
{
	for (char& ch : name)
	{
		if (!IsCharAlphaNumericA(ch))
		{
			ch = '_';
		}
	}

	return name;
}

std::vector<export_symbol> enumerate_exports(HMODULE moduleHandle)
{
	std::vector<export_symbol> symbols;

	// Microsoft documentation states that "When you specify ordinal values, they must be
	// in the range 1 through N, where N is the number of functions exported by the DLL."
	// This, however, is incorrect, and many system DLLs do not follow the rule.
	auto enum_proc = [](PVOID pContext, ULONG nOrdinal, LPCSTR pszName, PVOID pCode) -> BOOL
	{
		auto* symbols = static_cast<std::vector<export_symbol>*>(pContext);
		if(pszName)
		{
			symbols->push_back({ nOrdinal, pszName, get_safe_name(pszName), true});
		}
		else
		{
			const std::string dummyName = "Ordinal" + std::to_string(nOrdinal);
			symbols->push_back({ nOrdinal, dummyName, dummyName, false });
		}
		return true;
	};

	DetourEnumerateExports(moduleHandle, &symbols, enum_proc);
	return symbols;
}

bool generate_module_definition(const std::string& dllName, const std::vector<export_symbol>& exports)
{
	std::ofstream file;
	file.open(dllName + ".def");
	if(!file.is_open())
		return false;

	file << "LIBRARY " << dllName << "\n";
	file << "EXPORTS\n";

	for(const auto& symbol : exports)
	{
		file << '\t' << symbol.name << "=lib_" << dllName << '_' << symbol.safe_name << " @" << symbol.ordinal << (symbol.has_name ? "\n" : " NONAME\n");
	}

	return true;
}

bool generate_cpp_file(const std::string& dllName, const std::vector<export_symbol>& exports)
{
	std::string ucName{ dllName };
	CharUpperA(&ucName[0]);

	std::ofstream file;
	file.open(dllName + ".cpp");
	if(!file.is_open())
		return false;

	file << "#include \"pch.h\"\n\n";
	file << "#ifdef UIF_LIB_" << ucName << "\n\n";
	file << "#include \"injector.h\"\n\n";
	file << "struct {\n";
	file << "\tHMODULE dll;\n";

	for(const auto& symbol : exports)
	{
		file << "\tFARPROC " << symbol.safe_name << ";\n";
	}

	file << "} " << dllName << ";\n\n";

	for(const auto& symbol : exports)
	{
		file << "void lib_" << dllName << '_' << symbol.safe_name << "() { " << dllName << '.' << symbol.safe_name << "(); }\n";
	}

	file << "\nbool load_library_" << dllName << "() {\n";
	file << '\t' << dllName << ".dll = uif::injector::instance().load_real_library(\"" << dllName << ".dll\");\n";

	for(const auto& symbol : exports)
	{
		file << '\t' << dllName << '.' << symbol.safe_name << " = GetProcAddress(" << dllName << ".dll, ";
		if(symbol.has_name)
		{
			file << '"' << symbol.safe_name << "\");\n";
		}
		else
		{
			file << "MAKEINTRESOURCEA(" << symbol.ordinal << "));\n";
		}
	}

	file << "\treturn true;\n";
	file << "}\n\n";
	file << "bool unload_library_" << dllName << "() {\n";
	file << "\tFreeLibrary(" << dllName << ".dll);\n";
	file << "\t" << dllName << ".dll = nullptr;\n";
	file << "\treturn true;\n";
	file << "}\n\n";
	file << "#else\n\n";
	file << "bool load_library_" << dllName << "() {\n";
	file << "\treturn false;\n";
	file << "}\n\n";
	file << "bool unload_library_" << dllName << "() {\n";
	file << "\treturn false;\n";
	file << "}\n\n";
	file << "#endif\n";

	return true;
}

bool generate_header_file(const std::string& dllName, const std::vector<export_symbol>& exports)
{
	std::ofstream file;
	file.open(dllName + ".h");
	if(!file.is_open())
		return false;

	file << "#pragma once\n\n";
	file << "bool load_library_" << dllName << "();\n";
	file << "bool unload_library_" << dllName << "();\n";

	return true;
}

bool generate_files(const std::string& dllPath, const std::vector<export_symbol>& exports)
{
	const auto startIndex = dllPath.find_last_of("\\/") + 1;
	auto endIndex = dllPath.find_last_of('.');
	if(endIndex == std::string::npos) endIndex = dllPath.length();

	const std::string dllName = get_safe_name(dllPath.substr(startIndex, endIndex - startIndex));

	if(!generate_module_definition(dllName, exports))
		return false;

	if(!generate_cpp_file(dllName, exports))
		return false;

	if(!generate_header_file(dllName, exports))
		return false;

	return true;
}

int main(int argc, const char** argv)
{
	if(argc < 2)
	{
		std::cout << "Usage: ProxyGenerator.exe \"path\\to\\library.dll\"\n";
		std::cin.get();
		return 1;
	}

	const auto dllPath = argv[1];
	const auto hModule = load_library(dllPath);

	if(!hModule)
	{
		std::cout << "Unable to load " << dllPath << "\n";
		return 1;
	}

	const auto exports = enumerate_exports(hModule);

	for(const auto& symbol : exports) {
		std::cout << symbol.ordinal << ": " << symbol.name << "\n";
	}

	if(!generate_files(dllPath, exports))
	{
		std::cout << "Failed to generate files\n";
		return 1;
	}

	return 0;
}

