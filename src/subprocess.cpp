#if !defined(GB_SYSTEM_WINDOWS)
#include <spawn.h>
extern char **environ;
#endif

gb_internal void add_arg_fmt(Array<String> *args, char const *fmt, ...) {
	char buf[4096] = {0};

	va_list va;
	va_start(va, fmt);
	isize len = gb_snprintf_va(buf, gb_count_of(buf)-1, fmt, va)-1;
	va_end(va);

	array_add(args, copy_string(permanent_allocator(), make_string(cast(u8 *)buf, len)));
}

gb_internal Array<String> split_flags_string(gbAllocator a, String const &flags) {
	auto args = array_make<String>(a, 0, 8);

	isize i = 0;
	while (i < flags.len) {
		while (i < flags.len && gb_char_is_space(flags[i])) {
			i += 1;
		}
		if (i == flags.len) {
			break;
		}

		gbString arg = gb_string_make(a, "");
		u8 quote = 0;
		for (; i < flags.len; i++) {
			u8 c = flags[i];
			if (quote) {
				if (c == quote) {
					quote = 0;
				} else {
					arg = gb_string_append_length(arg, &c, 1);
				}
			} else if (c == '"' || c == '\'') {
				quote = c;
			} else if (gb_char_is_space(c)) {
				break;
			} else {
				arg = gb_string_append_length(arg, &c, 1);
			}
		}

		array_add(&args, make_string(cast(u8 *)arg, gb_string_length(arg)));
	}

	return args;
}

gb_internal void print_subprocess_args(FILE *f, Slice<String> const &args) {
	for (isize i = 0; i < args.count; i++) {
		String arg = args[i];

		bool needs_quoting = arg.len == 0;
		for (isize j = 0; !needs_quoting && j < arg.len; j++) {
			needs_quoting = gb_char_is_space(arg[j]) || arg[j] == '"' || arg[j] == '\'';
		}

		if (needs_quoting) {
			fprintf(f, "\"%.*s\"", LIT(arg));
		} else {
			fprintf(f, "%.*s", LIT(arg));
		}
		if (i+1 != args.count) {
			fprintf(f, " ");
		}
	}
	fprintf(f, "\n");
}


#if defined(GB_SYSTEM_WINDOWS)

gb_internal gbString win32_append_quoted_arg(gbString cmd_line, String const &arg) {
	bool needs_quoting = arg.len == 0;
	for (isize i = 0; !needs_quoting && i < arg.len; i++) {
		switch (arg[i]) {
		case ' ': case '\t': case '\n': case '\v': case '\"':
			needs_quoting = true;
			break;
		}
	}

	if (!needs_quoting) {
		return gb_string_append_length(cmd_line, arg.text, arg.len);
	}

	cmd_line = gb_string_append_length(cmd_line, "\"", 1);
	for (isize i = 0; i < arg.len; i++) {
		isize backslashes = 0;
		while (i < arg.len && arg[i] == '\\') {
			backslashes += 1;
			i += 1;
		}

		if (i == arg.len) {
			for (isize j = 0; j < 2*backslashes; j++) {
				cmd_line = gb_string_append_length(cmd_line, "\\", 1);
			}
			break;
		} else if (arg[i] == '\"') {
			for (isize j = 0; j < 2*backslashes+1; j++) {
				cmd_line = gb_string_append_length(cmd_line, "\\", 1);
			}
		} else {
			for (isize j = 0; j < backslashes; j++) {
				cmd_line = gb_string_append_length(cmd_line, "\\", 1);
			}
		}
		cmd_line = gb_string_append_length(cmd_line, &arg[i], 1);
	}
	cmd_line = gb_string_append_length(cmd_line, "\"", 1);
	return cmd_line;
}

gb_internal gbString win32_build_command_line(gbAllocator a, String const &name, Slice<String> const &args) {
	gbString cmd_line = gb_string_make(a, "");
	cmd_line = win32_append_quoted_arg(cmd_line, name);
	for (isize i = 0; i < args.count; i++) {
		cmd_line = gb_string_append_length(cmd_line, " ", 1);
		cmd_line = win32_append_quoted_arg(cmd_line, args[i]);
	}
	return cmd_line;
}

gb_internal bool win32_create_pipe(HANDLE *read_, HANDLE *write_) {
	SECURITY_ATTRIBUTES sa = {gb_size_of(SECURITY_ATTRIBUTES)};
	sa.bInheritHandle = true;

	if (!CreatePipe(read_, write_, &sa, 0)) {
		return false;
	}
	// Only the write end is meant to end up in the child.
	SetHandleInformation(*read_, HANDLE_FLAG_INHERIT, 0);
	return true;
}

gb_internal void win32_read_all(HANDLE handle, gbString *out_) {
	u8 buffer[1024];
	for (;;) {
		DWORD read = 0;
		if (!ReadFile(handle, buffer, gb_size_of(buffer), &read, nullptr) || read == 0) {
			return;
		}
		*out_ = gb_string_append_length(*out_, buffer, read);
	}
}

gb_internal i32 win32_run_subprocess(String const &name, Slice<String> const &args, bool from_path, gbString *stdout_, gbString *stderr_) {
	gbAllocator a = heap_allocator();

	gbString cmd_line = win32_build_command_line(a, name, args);
	defer (gb_string_free(cmd_line));

	String16 wcmd_line = string_to_string16(a, make_string(cast(u8 *)cmd_line, gb_string_length(cmd_line)));
	defer (gb_free(a, wcmd_line.text));

	String16 wname = {};
	if (!from_path) {
		wname = string_to_string16(a, name);
	}
	defer (gb_free(a, wname.text));

	STARTUPINFOW start_info = {gb_size_of(STARTUPINFOW)};
	PROCESS_INFORMATION pi = {0};

	HANDLE stdout_read = nullptr, stdout_write = nullptr;
	HANDLE stderr_read = nullptr, stderr_write = nullptr;
	defer ({
		if (stdout_read)  CloseHandle(stdout_read);
		if (stdout_write) CloseHandle(stdout_write);
		if (stderr_read)  CloseHandle(stderr_read);
		if (stderr_write) CloseHandle(stderr_write);
	});

	if (stdout_ || stderr_) {
		start_info.dwFlags   |= STARTF_USESTDHANDLES;
		start_info.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
		start_info.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
		start_info.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

		if (stdout_ && !win32_create_pipe(&stdout_read, &stdout_write)) {
			gb_printf_err("Failed to create pipe for command:\n\t%s\n", cmd_line);
			return -1;
		}
		if (stderr_ && !win32_create_pipe(&stderr_read, &stderr_write)) {
			gb_printf_err("Failed to create pipe for command:\n\t%s\n", cmd_line);
			return -1;
		}

		if (stdout_write) start_info.hStdOutput = stdout_write;
		if (stderr_write) start_info.hStdError  = stderr_write;
	}

	if (!CreateProcessW(cast(wchar_t *)wname.text, cast(wchar_t *)wcmd_line.text,
	                    nullptr, nullptr, true, 0, nullptr, nullptr,
	                    &start_info, &pi)) {
		gb_printf_err("Failed to execute command:\n\t%s\n", cmd_line);
		return -1;
	}

	// NOTE: the write ends have to be closed here, reading below would otherwise never see the end
	// of the stream because this process would still be holding one open
	if (stdout_write) { CloseHandle(stdout_write); stdout_write = nullptr; }
	if (stderr_write) { CloseHandle(stderr_write); stderr_write = nullptr; }

	// NOTE: drained before waiting, a child that fills up the pipe buffer would otherwise block
	// forever on a write that is never read
	if (stdout_read) win32_read_all(stdout_read, stdout_);
	if (stderr_read) win32_read_all(stderr_read, stderr_);

	i32 exit_code = 0;
	WaitForSingleObject(pi.hProcess, INFINITE);
	GetExitCodeProcess(pi.hProcess, cast(DWORD *)&exit_code);

	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return exit_code;
}

#else

gb_internal char **posix_build_argv(gbAllocator a, String const &name, Slice<String> const &args) {
	char **argv = gb_alloc_array(a, char *, args.count + 2);
	argv[0] = alloc_cstring(a, name);
	for (isize i = 0; i < args.count; i++) {
		argv[i+1] = alloc_cstring(a, args[i]);
	}
	argv[args.count + 1] = nullptr;
	return argv;
}

gb_internal void posix_free_argv(gbAllocator a, char **argv, isize arg_count) {
	for (isize i = 0; i < arg_count + 1; i++) {
		gb_free(a, argv[i]);
	}
	gb_free(a, argv);
}

gb_internal void posix_read_all(int fd, gbString *out_) {
	u8 buffer[1024];
	for (;;) {
		isize n = read(fd, buffer, gb_size_of(buffer));
		if (n <= 0) {
			return;
		}
		*out_ = gb_string_append_length(*out_, buffer, n);
	}
}

gb_internal bool posix_redirect_to_pipe(posix_spawn_file_actions_t *file_actions, int pipe_fds[2], int fd) {
	if (pipe(pipe_fds) != 0) {
		gb_printf_err("Could not create pipe for subprocess: %s\n", strerror(errno));
		return false;
	}
	posix_spawn_file_actions_addclose(file_actions, pipe_fds[0]);
	posix_spawn_file_actions_adddup2(file_actions, pipe_fds[1], fd);
	posix_spawn_file_actions_addclose(file_actions, pipe_fds[1]);
	return true;
}

gb_internal i32 posix_wait_for_subprocess(pid_t pid, bool raise_signal) {
	for (;;) {
		int status;
		if (waitpid(pid, &status, WUNTRACED) < 0) {
			gb_printf_err("Could not wait on subprocess: (pid: %d): %s\n", pid, strerror(errno));
			return -1;
		}

		if (WIFEXITED(status)) {
			return WEXITSTATUS(status);
		} else if (WIFSIGNALED(status)) {
			if (raise_signal) {
				struct rlimit limit = { 0, 0, };
				setrlimit(RLIMIT_CORE, &limit);
				raise(WTERMSIG(status));
			}
			return 128 + WTERMSIG(status);
		} else if (WIFSTOPPED(status)) {
			return -1;
		}
	}
	GB_PANIC("Subprocess failure");
	return -1;
}

gb_internal i32 posix_run_subprocess(String const &name, Slice<String> const &args, bool from_path, gbString *stdout_, gbString *stderr_, bool raise_signal) {
	gbAllocator a = heap_allocator();

	char **argv = posix_build_argv(a, name, args);
	defer (posix_free_argv(a, argv, args.count));

	int stdout_pipe[2] = {-1, -1};
	int stderr_pipe[2] = {-1, -1};
	defer ({
		if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
		if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
		if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
		if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
	});

	posix_spawn_file_actions_t file_actions;
	posix_spawn_file_actions_init(&file_actions);
	defer (posix_spawn_file_actions_destroy(&file_actions));

	if (stdout_ && !posix_redirect_to_pipe(&file_actions, stdout_pipe, STDOUT_FILENO)) {
		return -1;
	}
	if (stderr_ && !posix_redirect_to_pipe(&file_actions, stderr_pipe, STDERR_FILENO)) {
		return -1;
	}

	pid_t pid;
	int status;
	if (from_path) {
		status = posix_spawnp(&pid, argv[0], &file_actions, NULL, argv, environ);
	} else {
		status = posix_spawn(&pid, argv[0], &file_actions, NULL, argv, environ);
	}
	if (status != 0) {
		gb_printf_err("Could not spawn subprocess: %s\n", strerror(status));
		return -1;
	}

	// NOTE: the write ends have to be closed here, reading below would otherwise never see the end
	// of the stream because this process would still be holding one open
	if (stdout_pipe[1] >= 0) { close(stdout_pipe[1]); stdout_pipe[1] = -1; }
	if (stderr_pipe[1] >= 0) { close(stderr_pipe[1]); stderr_pipe[1] = -1; }

	// NOTE: drained before waiting, a child that fills up the pipe buffer would otherwise block
	// forever on a write that is never read
	if (stdout_pipe[0] >= 0) posix_read_all(stdout_pipe[0], stdout_);
	if (stderr_pipe[0] >= 0) posix_read_all(stderr_pipe[0], stderr_);

	return posix_wait_for_subprocess(pid, raise_signal);
}

#endif

gb_internal i32 run_subprocess_internal(String const &name, Slice<String> const &args, bool from_path, gbString *stdout_, gbString *stderr_, bool raise_signal) {
	if (build_context.show_system_calls) {
		gb_printf_err("[SYSTEM CALL] %.*s\n", LIT(name));
		print_subprocess_args(stderr, args);
		gb_printf_err("\n");
	}

#if defined(GB_SYSTEM_WINDOWS)
	return win32_run_subprocess(name, args, from_path, stdout_, stderr_);
#else
	return posix_run_subprocess(name, args, from_path, stdout_, stderr_, raise_signal);
#endif
}

gb_internal i32 run_subprocess(String const &name, Slice<String> const &args, bool from_path, bool raise_signal = false) {
	if (build_context.print_linker_flags) {
		// NOTE: the executable itself is not a linker flag
		print_subprocess_args(stdout, args);
		return 0;
	}

	return run_subprocess_internal(name, args, from_path, nullptr, nullptr, raise_signal);
}

// Runs a subprocess the caller needs an answer from, capturing its output rather than inheriting
// this process' handles. The caller is responsible for freeing what it asked for.
gb_internal i32 capture_subprocess(String const &name, Slice<String> const &args, bool from_path, gbString *stdout_, gbString *stderr_ = nullptr) {
	return run_subprocess_internal(name, args, from_path, stdout_, stderr_, /*raise_signal*/false);
}
