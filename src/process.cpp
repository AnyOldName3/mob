#include "pch.h"
#include "process.h"
#include "conf.h"
#include "net.h"
#include "context.h"
#include "op.h"

namespace mob
{

async_pipe::async_pipe()
	: pending_(false)
{
	std::memset(buffer_, 0, sizeof(buffer_));
	std::memset(&ov_, 0, sizeof(ov_));
}

handle_ptr async_pipe::create()
{
	// creating pipe
	handle_ptr out(create_pipe());
	if (out.get() == INVALID_HANDLE_VALUE)
		return {};

	ov_.hEvent = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);

	if (ov_.hEvent == NULL)
	{
		const auto e = GetLastError();
		bail_out("CreateEvent failed", e);
	}

	event_.reset(ov_.hEvent);

	return out;
}

std::string_view async_pipe::read()
{
	if (pending_)
		return check_pending();
	else
		return try_read();
}

HANDLE async_pipe::create_pipe()
{
	static std::atomic<int> pipe_id(0);

	const std::string pipe_name_prefix = "\\\\.\\pipe\\mob_pipe";
	const std::string pipe_name = pipe_name_prefix + std::to_string(++pipe_id);

	SECURITY_ATTRIBUTES sa = {};
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = TRUE;

	handle_ptr pipe;

	// creating pipe
	{
		HANDLE pipe_handle = ::CreateNamedPipeA(
			pipe_name.c_str(), PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED,
			PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT,
			1, 50'000, 50'000, pipe_timeout, &sa);

		if (pipe_handle == INVALID_HANDLE_VALUE)
		{
			const auto e = GetLastError();
			bail_out("CreateNamedPipe failed", e);
		}

		pipe.reset(pipe_handle);
	}

	{
		// duplicating the handle to read from it
		HANDLE output_read = INVALID_HANDLE_VALUE;

		const auto r = DuplicateHandle(
			GetCurrentProcess(), pipe.get(), GetCurrentProcess(), &output_read,
			0, TRUE, DUPLICATE_SAME_ACCESS);

		if (!r)
		{
			const auto e = GetLastError();
			bail_out("DuplicateHandle for pipe", e);
		}

		stdout_.reset(output_read);
	}


	// creating handle to pipe which is passed to CreateProcess()
	HANDLE output_write = ::CreateFileA(
		pipe_name.c_str(), FILE_WRITE_DATA|SYNCHRONIZE, 0,
		&sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);

	if (output_write == INVALID_HANDLE_VALUE)
	{
		const auto e = GetLastError();
		bail_out("CreateFileW for pipe failed", e);
	}

	return output_write;
}

std::string_view async_pipe::try_read()
{
	DWORD bytes_read = 0;

	if (!::ReadFile(stdout_.get(), buffer_, buffer_size, &bytes_read, &ov_))
	{
		const auto e = GetLastError();

		switch (e)
		{
			case ERROR_IO_PENDING:
			{
				pending_ = true;
				break;
			}

			case ERROR_BROKEN_PIPE:
			{
				// broken pipe probably means lootcli is finished
				break;
			}

			default:
			{
				bail_out("async_pipe read failed", e);
				break;
			}
		}

		return {};
	}

	return {buffer_, bytes_read};
}

std::string_view async_pipe::check_pending()
{
	DWORD bytes_read = 0;

	const auto r = WaitForSingleObject(event_.get(), pipe_timeout);

	if (r == WAIT_FAILED) {
		const auto e = GetLastError();
		bail_out("WaitForSingleObject in async_pipe failed", e);
	}

	if (!::GetOverlappedResult(stdout_.get(), &ov_, &bytes_read, FALSE))
	{
		const auto e = GetLastError();

		switch (e)
		{
			case ERROR_IO_INCOMPLETE:
			{
				break;
			}

			case WAIT_TIMEOUT:
			{
				break;
			}

			case ERROR_BROKEN_PIPE:
			{
				// broken pipe probably means lootcli is finished
				break;
			}

			default:
			{
				bail_out("GetOverlappedResult failed in async_pipe", e);
				break;
			}
		}

		return {};
	}

	::ResetEvent(event_.get());
	pending_ = false;

	return {buffer_, bytes_read};
}


process::impl::impl(const impl& i)
	: interrupt(i.interrupt.load())
{
}

process::impl& process::impl::operator=(const impl& i)
{
	interrupt = i.interrupt.load();
	return *this;
}


process::process(const context* cx) :
	cx_(cx ? cx : &gcx()), flags_(process::noflags),
	stdout_level_(context::level::trace),
	stderr_level_(context::level::error),
	code_(0)
{
}

process::~process()
{
	try
	{
		join();
	}
	catch(...)
	{
	}
}

process process::raw(const std::string& cmd)
{
	process p;
	p.raw_ = cmd;
	return p;
}

process& process::set_context(const context* cx)
{
	cx_ = cx;
	return *this;
}

process& process::name(const std::string& name)
{
	name_ = name;
	return *this;
}

const std::string& process::name() const
{
	return name_;
}

process& process::binary(const fs::path& p)
{
	bin_ = p;
	return *this;
}

const fs::path& process::binary() const
{
	return bin_;
}

process& process::cwd(const fs::path& p)
{
	cwd_ = p;
	return *this;
}

const fs::path& process::cwd() const
{
	return cwd_;
}

process& process::stdout_level(context::level lv)
{
	stdout_level_ = lv;
	return *this;
}

process& process::stdout_filter(filter_fun f)
{
	stdout_filter_ = f;
	return *this;
}

process& process::stderr_level(context::level lv)
{
	stderr_level_ = lv;
	return *this;
}

process& process::stderr_filter(filter_fun f)
{
	stderr_filter_ = f;
	return *this;
}

process& process::flags(flags_t f)
{
	flags_ = f;
	return *this;
}

process::flags_t process::flags() const
{
	return flags_;
}

process& process::env(const mob::env& e)
{
	env_ = e;
	return *this;
}

std::string process::make_name() const
{
	if (!name_.empty())
		return name_;

	return make_cmd();
}

std::string process::make_cmd() const
{
	if (!raw_.empty())
		return raw_;

	return "\"" + bin_.string() + "\"" + cmd_;
}

void process::pipe_into(const process& p)
{
	raw_ = make_cmd() + " | " + p.make_cmd();
}

void process::run()
{
	if (!cwd_.empty())
		cx_->debug(context::cmd, "> cd " + cwd_.string());

	const auto what = make_cmd();
	cx_->debug(context::cmd, "> " + what);

	if (conf::dry())
		return;

	do_run(what);
}

void process::do_run(const std::string& what)
{
	STARTUPINFOA si = { .cb=sizeof(si) };
	PROCESS_INFORMATION pi = {};

	auto process_stdout = impl_.stdout_pipe.create();
	si.hStdOutput = process_stdout.get();

	auto process_stderr = impl_.stderr_pipe.create();
	si.hStdError = process_stderr.get();

	si.dwFlags = STARTF_USESTDHANDLES;

	const std::string cmd = this_env::get("COMSPEC");
	const std::string args = "/C \"" + what + "\"";

	const char* cwd_p = nullptr;
	std::string cwd_s;

	if (!cwd_.empty())
	{
		op::create_directories(*cx_, cwd_);
		cwd_s = cwd_.string();
		cwd_p = (cwd_s.empty() ? nullptr : cwd_s.c_str());
	}

	cx_->trace(context::cmd, "creating process");

	const auto r = ::CreateProcessA(
		cmd.c_str(), const_cast<char*>(args.c_str()),
		nullptr, nullptr, TRUE, CREATE_NEW_PROCESS_GROUP,
		env_.get_pointers(), cwd_p, &si, &pi);

	if (!r)
	{
		const auto e = GetLastError();
		cx_->bail_out(context::cmd, "failed to start '" + cmd + "'", e);
	}

	cx_->trace(context::cmd, "pid " + std::to_string(pi.dwProcessId));

	::CloseHandle(pi.hThread);
	impl_.handle.reset(pi.hProcess);
}

void process::interrupt()
{
	impl_.interrupt = true;
	cx_->trace(context::cmd, "will interrupt");
}

void process::join()
{
	if (!impl_.handle)
		return;

	bool interrupted = false;

	cx_->trace(context::cmd, "joining");

	for (;;)
	{
		const auto r = WaitForSingleObject(impl_.handle.get(), 100);

		if (r == WAIT_OBJECT_0)
		{
			// done
			read_pipes();

			GetExitCodeProcess(impl_.handle.get(), &code_);

			cx_->debug(context::cmd,
				"process completed, exit code " + std::to_string(code_));

			if (impl_.interrupt)
				break;

			if (code_ != 0)
			{
				if (flags_ & allow_failure)
				{
					cx_->debug(context::cmd,
						"process failed but failure was allowed");
				}
				else
				{
					impl_.handle = {};

					cx_->bail_out(context::cmd,
						make_name() + " returned " + std::to_string(code_));
				}
			}

			break;
		}

		if (r == WAIT_TIMEOUT)
		{
			read_pipes();

			if (impl_.interrupt && !interrupted)
			{
				if (flags_ & terminate_on_interrupt)
				{
					cx_->trace(context::cmd,
						"terminating process (flag is set)");

					::TerminateProcess(impl_.handle.get(), 0xffff);

					break;
				}
				else
				{
					const auto pid = GetProcessId(impl_.handle.get());

					if (pid == 0)
					{
						cx_->error(context::cmd,
							"process id is 0, terminating instead");

						::TerminateProcess(impl_.handle.get(), 0xffff);

						break;
					}
					else
					{
						cx_->error(context::cmd,
							"sending sigint to " + std::to_string(pid));

						GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid);
					}
				}

				interrupted = true;
			}

			continue;
		}

		const auto e = GetLastError();
		impl_.handle = {};
		cx_->bail_out(context::cmd, "failed to wait on process", e);
	}

	if (interrupted)
		cx_->trace(context::cmd, "process interrupted and finished");

	impl_.handle = {};
}

void process::read_pipes()
{
	std::string_view s = impl_.stdout_pipe.read();
	for_each_line(s, [&](auto&& line)
	{
		filter f = {line, context::std_out, stdout_level_, false};

		if (stdout_filter_)
		{
			stdout_filter_(f);
			if (f.ignore)
				return;
		}

		cx_->log(f.r, f.lv, f.line);
	});

	s = impl_.stderr_pipe.read();
	for_each_line(s, [&](auto&& line)
	{
		filter f = {line, context::std_err, stderr_level_, false};

		if (stderr_filter_)
		{
			stderr_filter_(f);
			if (f.ignore)
				return;
		}

		cx_->log(f.r, f.lv, f.line);
	});
}

int process::exit_code() const
{
	return static_cast<int>(code_);
}

void process::add_arg(const std::string& k, const std::string& v, arg_flags f)
{
	if ((f & verbose) && !conf::log_trace())
		return;

	if ((f & quiet) && conf::log_trace())
		return;

	if (k.empty() && v.empty())
		return;

	if (k.empty())
		cmd_ += " " + v;
	else if ((f & nospace) || k.back() == '=')
		cmd_ += " " + k + v;
	else
		cmd_ += " " + k + " " + v;
}

std::string process::arg_to_string(const char* s, bool force_quote)
{
	if (force_quote)
		return "\"" + std::string(s) + "\"";
	else
		return s;
}

std::string process::arg_to_string(const std::string& s, bool force_quote)
{
	if (force_quote)
		return "\"" + std::string(s) + "\"";
	else
		return s;
}

std::string process::arg_to_string(const fs::path& p, bool)
{
	return "\"" + p.string() + "\"";
}

std::string process::arg_to_string(const url& u, bool force_quote)
{
	if (force_quote)
		return "\"" + u.string() + "\"";
	else
		return u.string();
}

}	// namespace
