#include "pch.h"
#include "tasks.h"

namespace mob
{

explorerpp::explorerpp()
	: basic_task("explorerpp", "explorer++")
{
}

fs::path explorerpp::source_path()
{
	return paths::build() / "explorer++";
}

void explorerpp::do_fetch()
{
	const auto file = run_tool(downloader(source_url()));

	run_tool(extractor()
		.file(file)
		.output(source_path()));

	op::copy_glob_to_dir_if_better(cx(),
		source_path() / "*",
		paths::install_bin() / "explorer++",
		op::copy_files);
}

url explorerpp::source_url()
{
	return
		"https://explorerplusplus.com/software/"
		"explorer++_" + versions::explorerpp() + "_x64.zip";
}

}	// namespace