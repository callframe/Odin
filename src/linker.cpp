struct LinkerData {
	BlockingMutex foreign_mutex;
	PtrSet<Entity *> foreign_libraries_set;
	Array<Entity *>  foreign_libraries;

	Array<String> output_object_paths;
	Array<String> output_temp_paths;
	String   output_base;
	String   output_name;
};

gb_internal void linker_data_init(LinkerData *ld, CheckerInfo *info, String const &init_fullpath) {
	gbAllocator ha = heap_allocator();
	array_init(&ld->output_object_paths, ha);
	array_init(&ld->output_temp_paths,   ha);
	array_init(&ld->foreign_libraries,   ha, 0, 1024);
	ptr_set_init(&ld->foreign_libraries_set, 1024);

	if (build_context.out_filepath.len == 0) {
		ld->output_name = remove_directory_from_path(init_fullpath);
		ld->output_name = remove_extension_from_path(ld->output_name);
		ld->output_name = string_trim_whitespace(ld->output_name);
		if (ld->output_name.len == 0) {
			ld->output_name = info->init_scope->pkg->name;
		}
		ld->output_base = ld->output_name;
	} else {
		ld->output_name = build_context.out_filepath;
		ld->output_name = string_trim_whitespace(ld->output_name);
		if (ld->output_name.len == 0) {
			ld->output_name = info->init_scope->pkg->name;
		}
		isize pos = string_extension_position(ld->output_name);
		if (pos < 0) {
			ld->output_base = ld->output_name;
		} else {
			ld->output_base = substring(ld->output_name, 0, pos);
		}
	}

	ld->output_base = path_to_full_path(ha, ld->output_base);

}

gb_internal i32 linker_stage(LinkerData *gen) {
	i32 result = 0;
	Timings *timings = &global_timings;

	String output_filename = path_to_string(heap_allocator(), build_context.build_paths[BuildPath_Output]);
	debugf("Linking %.*s\n", LIT(output_filename));

	// TOOD(Jeroen): Make a `build_paths[BuildPath_Object] to avoid `%.*s.o`.

	if (is_arch_wasm()) {
		timings_start_section(timings, str_lit("wasm-ld"));

		auto lib_args = array_make<String>(temporary_allocator(), 0, 16);

		auto extra_orca_args = array_make<String>(temporary_allocator(), 0, 8);

		auto inputs = array_make<String>(temporary_allocator(), 0, 16);
		add_arg_fmt(&inputs, "%.*s.o", LIT(output_filename));


		for (Entity *e : gen->foreign_libraries) {
			GB_ASSERT(e->kind == Entity_LibraryName);
			// NOTE(bill): Add these before the linking values
			String extra_linker_flags = string_trim_whitespace(e->LibraryName.extra_linker_flags);
			if (extra_linker_flags.len != 0) {
				for (String const &flag : split_flags_string(temporary_allocator(), extra_linker_flags)) {
					array_add(&lib_args, flag);
				}
			}

			for_array(i, e->LibraryName.paths) {
				String lib = e->LibraryName.paths[i];

				if (lib.len == 0) {
					continue;
				}

				if (!string_ends_with(lib, str_lit(".o"))) {
					continue;
				}

				array_add(&inputs, lib);
			}
		}

		if (build_context.metrics.os == TargetOs_orca) {
			gbString orca_sdk_path = gb_string_make(temporary_allocator(), "");

			auto orca_args = array_make<String>(temporary_allocator(), 0, 1);
			array_add(&orca_args, str_lit("sdk-path"));

			if (capture_subprocess(str_lit("orca"), slice_from_array(orca_args), true, &orca_sdk_path) != 0) {
				gb_printf_err("executing `orca sdk-path` failed, make sure Orca is installed and added to your path\n");
				return 1;
			}
			orca_sdk_path = gb_string_trim_space(orca_sdk_path);
			if (gb_string_length(orca_sdk_path) == 0) {
				gb_printf_err("executing `orca sdk-path` did not produce output\n");
				return 1;
			}
			add_arg_fmt(&inputs, "%s/orca-libc/lib/crt1.o", orca_sdk_path);
			add_arg_fmt(&inputs, "%s/orca-libc/lib/libc.a", orca_sdk_path);

			array_add(&extra_orca_args, str_lit("-L"));
			add_arg_fmt(&extra_orca_args, "%s/bin", orca_sdk_path);
			array_add(&extra_orca_args, str_lit("-lorca_wasm"));
			array_add(&extra_orca_args, str_lit("--export-dynamic"));
		}

		auto args = array_make<String>(temporary_allocator(), 0, 32);
		array_add_elems(&args, inputs.data, inputs.count);
		array_add(&args, str_lit("-o"));
		array_add(&args, output_filename);
		array_add_elems(&args, build_context.link_flags.data, build_context.link_flags.count);
		for (String const &flag : split_flags_string(temporary_allocator(), build_context.extra_linker_flags)) {
			array_add(&args, flag);
		}
		array_add_elems(&args, lib_args.data, lib_args.count);
		array_add_elems(&args, extra_orca_args.data, extra_orca_args.count);

	#if defined(GB_SYSTEM_WINDOWS)
		String wasm_ld = concatenate_strings(temporary_allocator(), build_context.ODIN_ROOT, str_lit("bin\\wasm-ld"));
		result = run_subprocess(wasm_ld, slice_from_array(args), false);
	#else
		result = run_subprocess(str_lit("wasm-ld"), slice_from_array(args), true);
	#endif
		return result;
	}

	bool is_cross_linking = false;
	bool is_android = false;

	if (build_context.cross_compiling && (build_context.different_os || selected_subtarget != Subtarget_Default)) {
		switch (selected_subtarget) {
		case Subtarget_Android:
			is_cross_linking = true;
			is_android = true;
			goto try_cross_linking;
		default:
			gb_printf_err("Linking for cross compilation for this platform is not yet supported (%.*s %.*s)\n",
				LIT(target_os_names[build_context.metrics.os]),
				LIT(target_arch_names[build_context.metrics.arch])
			);
			build_context.keep_object_files = true;
			break;
		}
	} else {
try_cross_linking:;

	#if defined(GB_SYSTEM_WINDOWS)
		String section_name = str_lit("msvc-link");
		bool is_windows = build_context.metrics.os == TargetOs_windows;
	#else
		String section_name = str_lit("ld-link");
		bool is_windows = false;
	#endif

		bool is_osx = build_context.metrics.os == TargetOs_darwin;


		switch (build_context.linker_choice) {
		case Linker_Default:  break;
		case Linker_lld:      section_name = str_lit("lld-link"); break;
	#if defined(GB_SYSTEM_LINUX) || defined(GB_SYSTEM_FREEBSD) || defined(GB_SYSTEM_NETBSD)
		case Linker_mold:     section_name = str_lit("mold-link"); break;
	#endif
	#if defined(GB_SYSTEM_WINDOWS)
		case Linker_radlink:  section_name = str_lit("rad-link"); break;
	#endif
		default:
			gb_printf_err("'%.*s' linker is not supported on this platform\n", LIT(linker_choices[build_context.linker_choice]));
			return 1;
		}


		if (is_windows) {
			timings_start_section(timings, section_name);

			auto lib_args = array_make<String>(temporary_allocator(), 0, 32);

			auto link_settings = array_make<String>(temporary_allocator(), 0, 16);

			// Add library search paths.
			if (build_context.build_paths[BuildPath_VS_LIB].basename.len > 0) {
				String path = {};
				auto add_path = [&](String path) {
					if (path[path.len-1] == '\\') {
						path.len -= 1;
					}
					add_arg_fmt(&link_settings, "/LIBPATH:%.*s", LIT(path));
				};
				add_path(build_context.build_paths[BuildPath_Win_SDK_UM_Lib].basename);
				add_path(build_context.build_paths[BuildPath_Win_SDK_UCRT_Lib].basename);
				add_path(build_context.build_paths[BuildPath_VS_LIB].basename);
			}


			StringSet min_libs_set = {};
			string_set_init(&min_libs_set, 64);
			defer (string_set_destroy(&min_libs_set));

			String prev_lib = {};

			StringSet asm_files = {};
			string_set_init(&asm_files, 64);
			defer (string_set_destroy(&asm_files));

			for (Entity *e : gen->foreign_libraries) {
				GB_ASSERT(e->kind == Entity_LibraryName);
				// NOTE(bill): Add these before the linking values
				String extra_linker_flags = string_trim_whitespace(e->LibraryName.extra_linker_flags);
				if (extra_linker_flags.len != 0) {
					for (String const &flag : split_flags_string(temporary_allocator(), extra_linker_flags)) {
						array_add(&lib_args, flag);
					}
				}
				for_array(i, e->LibraryName.paths) {
					String lib = string_trim_whitespace(e->LibraryName.paths[i]);
					// IMPORTANT NOTE(bill): calling `string_to_lower` here is not an issue because
					// we will never uses these strings afterwards
					string_to_lower(&lib);
					if (lib.len == 0) {
						continue;
					}

					if (has_asm_extension(lib)) {
						if (!string_set_update(&asm_files, lib)) {
							String asm_file = lib;
							String obj_file = {};
							String temp_dir = temporary_directory(temporary_allocator());
							if (temp_dir.len != 0) {
								String filename = filename_without_directory(asm_file);

								gbString str = gb_string_make(heap_allocator(), "");
								str = gb_string_append_length(str, temp_dir.text, temp_dir.len);
								str = gb_string_appendc(str, "/");
								str = gb_string_append_length(str, filename.text, filename.len);
								str = gb_string_append_fmt(str, "-%p.obj", asm_file.text);
								obj_file = make_string_c(str);
							} else {
								obj_file = concatenate_strings(permanent_allocator(), asm_file, str_lit(".obj"));
							}

							String obj_format = str_lit("win64");
						#if defined(GB_ARCH_32_BIT)
							obj_format = str_lit("win32");
						#endif

							String nasm_program = concatenate_strings(temporary_allocator(), build_context.ODIN_ROOT, str_lit("bin\\nasm\\windows\\nasm.exe"));

							auto nasm_args = array_make<String>(temporary_allocator(), 0, 8);
							array_add(&nasm_args, asm_file);
							array_add(&nasm_args, str_lit("-f"));
							array_add(&nasm_args, obj_format);
							array_add(&nasm_args, str_lit("-o"));
							array_add(&nasm_args, obj_file);
							for (String const &flag : split_flags_string(temporary_allocator(), build_context.extra_assembler_flags)) {
								array_add(&nasm_args, flag);
							}

							result = run_subprocess(nasm_program, slice_from_array(nasm_args), false);

							if (result) {
								return result;
							}
							array_add(&gen->output_object_paths, obj_file);
						}
					} else if (!string_set_update(&min_libs_set, lib) ||
					           !build_context.min_link_libs) {
						if (prev_lib != lib) {
							array_add(&lib_args, lib);
						}
						prev_lib = lib;
					}
				}
			}

			if (build_context.build_mode == BuildMode_DynamicLibrary) {
				array_add(&link_settings, str_lit("/DLL"));
				if (build_context.no_entry_point) {
					array_add(&link_settings, str_lit("/NOENTRY"));
				}
			} else {
				// For i386 with CRT, libcmt provides the entry point
				// For other cases or no_crt, we need to specify the entry point
				if (!(build_context.metrics.arch == TargetArch_i386 && !build_context.no_crt)) {
					array_add(&link_settings, str_lit("/ENTRY:mainCRTStartup"));
				}
			}

			if (build_context.build_paths[BuildPath_Symbols].name != "") {
				String symbol_path = path_to_string(heap_allocator(), build_context.build_paths[BuildPath_Symbols]);
				add_arg_fmt(&link_settings, "/PDB:%.*s", LIT(symbol_path));
			}

			if (build_context.build_mode != BuildMode_StaticLibrary) {
				if (build_context.no_crt) {
					array_add(&link_settings, str_lit("/nodefaultlib"));
				} else {
					array_add(&link_settings, str_lit("/defaultlib:libcmt"));
				}
			}

			if (build_context.ODIN_DEBUG) {
				array_add(&link_settings, str_lit("/DEBUG"));
			}

			auto object_files = array_make<String>(temporary_allocator(), 0, 32);
			for (String const &object_path : gen->output_object_paths) {
				array_add(&object_files, object_path);
			}

			String vs_exe_path = path_to_string(heap_allocator(), build_context.build_paths[BuildPath_VS_EXE]);
			defer (gb_free(heap_allocator(), vs_exe_path.text));

			String windows_sdk_bin_path = path_to_string(heap_allocator(), build_context.build_paths[BuildPath_Win_SDK_Bin_Path]);
			defer (gb_free(heap_allocator(), windows_sdk_bin_path.text));

			auto lld_lto_flags = array_make<String>(temporary_allocator(), 0, 2);
			if (build_context.lto_kind != LTO_None) {
				add_arg_fmt(&lld_lto_flags, "/opt:lldltojobs=%d", build_context.thread_count);
			}

			auto extra_link_args = split_flags_string(temporary_allocator(), build_context.extra_linker_flags);

			switch (build_context.linker_choice) {
			case Linker_lld: {
				String lld_program = concatenate_strings(temporary_allocator(), build_context.ODIN_ROOT, str_lit("bin\\lld-link"));

				auto args = array_make<String>(temporary_allocator(), 0, 64);
				array_add_elems(&args, object_files.data, object_files.count);
				add_arg_fmt(&args, "-OUT:%.*s", LIT(output_filename));
				array_add_elems(&args, link_settings.data, link_settings.count);
				array_add(&args, str_lit("/nologo"));
				array_add(&args, str_lit("/incremental:no"));
				array_add(&args, str_lit("/opt:ref"));
				add_arg_fmt(&args, "/subsystem:%.*s", LIT(windows_subsystem_names[build_context.ODIN_WINDOWS_SUBSYSTEM]));
				array_add_elems(&args, build_context.link_flags.data, build_context.link_flags.count);
				array_add_elems(&args, extra_link_args.data, extra_link_args.count);
				array_add_elems(&args, lib_args.data, lib_args.count);
				array_add_elems(&args, lld_lto_flags.data, lld_lto_flags.count);

				result = run_subprocess(lld_program, slice_from_array(args), false);

				if (result) {
					return result;
				}
				break;
			}
			case Linker_radlink: {
				String rad_program = concatenate_strings(temporary_allocator(), build_context.ODIN_ROOT, str_lit("bin\\radlink"));

				auto args = array_make<String>(temporary_allocator(), 0, 64);
				array_add_elems(&args, object_files.data, object_files.count);
				add_arg_fmt(&args, "-OUT:%.*s", LIT(output_filename));
				array_add_elems(&args, link_settings.data, link_settings.count);
				array_add(&args, str_lit("/nologo"));
				array_add(&args, str_lit("/incremental:no"));
				array_add(&args, str_lit("/opt:ref"));
				add_arg_fmt(&args, "/subsystem:%.*s", LIT(windows_subsystem_names[build_context.ODIN_WINDOWS_SUBSYSTEM]));
				array_add_elems(&args, build_context.link_flags.data, build_context.link_flags.count);
				array_add_elems(&args, extra_link_args.data, extra_link_args.count);
				array_add_elems(&args, lib_args.data, lib_args.count);

				result = run_subprocess(rad_program, slice_from_array(args), false);

				if (result) {
					return result;
				}
				break;
			}
			default: { // msvc
				String res_path = path_to_string(heap_allocator(), build_context.build_paths[BuildPath_RES]);
				String rc_path  = path_to_string(heap_allocator(), build_context.build_paths[BuildPath_RC]);
				defer (gb_free(heap_allocator(), res_path.text));
				defer (gb_free(heap_allocator(), rc_path.text));

				if (build_context.has_resource) {
					if (build_context.build_paths[BuildPath_RC].basename == "")  {
						debugf("Using precompiled resource %.*s\n", LIT(res_path));
					} else {
						debugf("Compiling resource %.*s\n", LIT(res_path));

						String rc_program = concatenate_strings(temporary_allocator(), windows_sdk_bin_path, str_lit("rc.exe"));

						auto rc_args = array_make<String>(temporary_allocator(), 0, 4);
						array_add(&rc_args, str_lit("/nologo"));
						array_add(&rc_args, str_lit("/fo"));
						array_add(&rc_args, res_path);
						array_add(&rc_args, rc_path);

						result = run_subprocess(rc_program, slice_from_array(rc_args), false);

						if (result) {
							return result;
						}
					}
				} else {
					res_path = {};
				}

				String linker_name = str_lit("link.exe");
				switch (build_context.build_mode) {
				case BuildMode_Executable:
					array_add(&link_settings, str_lit("/NOIMPLIB"));
					array_add(&link_settings, str_lit("/NOEXP"));
					break;
				}

				switch (build_context.build_mode) {
				case BuildMode_StaticLibrary:
					linker_name = str_lit("lib.exe");
					break;
				default:
					array_add(&link_settings, str_lit("/incremental:no"));
					array_add(&link_settings, str_lit("/opt:ref"));
					break;
				}

				String link_program = concatenate_strings(temporary_allocator(), vs_exe_path, linker_name);

				auto args = array_make<String>(temporary_allocator(), 0, 64);
				array_add_elems(&args, object_files.data, object_files.count);
				if (res_path.len != 0) {
					array_add(&args, res_path);
				}
				add_arg_fmt(&args, "-OUT:%.*s", LIT(output_filename));
				array_add_elems(&args, link_settings.data, link_settings.count);
				array_add(&args, str_lit("/nologo"));
				add_arg_fmt(&args, "/subsystem:%.*s", LIT(windows_subsystem_names[build_context.ODIN_WINDOWS_SUBSYSTEM]));
				array_add_elems(&args, build_context.link_flags.data, build_context.link_flags.count);
				array_add_elems(&args, extra_link_args.data, extra_link_args.count);
				array_add_elems(&args, lib_args.data, lib_args.count);

				result = run_subprocess(link_program, slice_from_array(args), false);
				if (result) {
					return result;
				}
				break;
			}
			}
		} else {

			timings_start_section(timings, section_name);

			int const ODIN_ANDROID_API_LEVEL = build_context.ODIN_ANDROID_API_LEVEL;

			String ODIN_ANDROID_NDK                     = build_context.ODIN_ANDROID_NDK;
			String ODIN_ANDROID_NDK_TOOLCHAIN           = build_context.ODIN_ANDROID_NDK_TOOLCHAIN;
			String ODIN_ANDROID_NDK_TOOLCHAIN_LIB       = build_context.ODIN_ANDROID_NDK_TOOLCHAIN_LIB;
			String ODIN_ANDROID_NDK_TOOLCHAIN_LIB_LEVEL = build_context.ODIN_ANDROID_NDK_TOOLCHAIN_LIB_LEVEL;
			String ODIN_ANDROID_NDK_TOOLCHAIN_SYSROOT   = build_context.ODIN_ANDROID_NDK_TOOLCHAIN_SYSROOT;

			// Link using `clang`, unless overridden by `ODIN_CLANG_PATH` environment variable.
			const char* clang_path_env = gb_get_env("ODIN_CLANG_PATH", permanent_allocator());
			bool has_odin_clang_path_env = true;
			String clang_program = make_string_c(clang_path_env);
			if (clang_path_env == NULL) {
				clang_program = str_lit("clang");
				has_odin_clang_path_env = false;
			}
			auto clang_prefix_args = array_make<String>(temporary_allocator(), 0, 4);

			// NOTE(vassvik): needs to add the root to the library search paths, so that the full filenames of the library
			//                files can be passed with -l:
			auto lib_args = array_make<String>(temporary_allocator(), 0, 32);
			#if !defined(GB_SYSTEM_WINDOWS)
				array_add(&lib_args, str_lit("-L/"));
			#endif

			StringSet asm_files = {};
			string_set_init(&asm_files, 64);
			defer (string_set_destroy(&asm_files));
			
			StringSet min_libs_set = {};
			string_set_init(&min_libs_set, 64);
			defer (string_set_destroy(&min_libs_set));

			String prev_lib = {};
			
			for (Entity *e : gen->foreign_libraries) {
				GB_ASSERT(e->kind == Entity_LibraryName);
				// NOTE(bill): Add these before the linking values
				String extra_linker_flags = string_trim_whitespace(e->LibraryName.extra_linker_flags);
				if (extra_linker_flags.len != 0) {
					for (String const &flag : split_flags_string(temporary_allocator(), extra_linker_flags)) {
						array_add(&lib_args, flag);
					}
				}

				if (build_context.metrics.os == TargetOs_darwin) {
					// Print frameworks first
					for (String lib : e->LibraryName.paths) {
						lib = string_trim_whitespace(lib);
						if (lib.len == 0) {
							continue;
						}
						if (string_ends_with(lib, str_lit(".framework"))) {
							if (string_set_update(&min_libs_set, lib)) {
								continue;
							}

							String lib_name = lib;
							lib_name = remove_extension_from_path(lib_name);
							array_add(&lib_args, str_lit("-framework"));
							array_add(&lib_args, lib_name);
						}
					}
				}

				for (String lib : e->LibraryName.paths) {
					lib = string_trim_whitespace(lib);
					if (lib.len == 0) {
						continue;
					}
					if (has_asm_extension(lib)) {
						if (string_set_update(&asm_files, lib)) {
							continue; // already handled
						}
						String asm_file = lib;
						String obj_file = {};

						String temp_dir = temporary_directory(temporary_allocator());
						if (temp_dir.len != 0) {
							String filename = filename_without_directory(asm_file);

							gbString str = gb_string_make(heap_allocator(), "");
							str = gb_string_append_length(str, temp_dir.text, temp_dir.len);
							str = gb_string_appendc(str, "/");
							str = gb_string_append_length(str, filename.text, filename.len);
							str = gb_string_append_fmt(str, "-%p.o", asm_file.text);
							obj_file = make_string_c(str);
						} else {
							obj_file = concatenate_strings(permanent_allocator(), asm_file, str_lit(".o"));
						}

						String obj_format;
						if (build_context.metrics.ptr_size == 8) {
							if (is_osx) {
								obj_format = str_lit("macho64");
							} else {
								obj_format = str_lit("elf64");
							}
						} else {
							GB_ASSERT(build_context.metrics.ptr_size == 4);
							if (is_osx) {
								obj_format = str_lit("macho32");
							} else {
								obj_format = str_lit("elf32");
							}
						}

						auto asm_args = array_make<String>(temporary_allocator(), 0, 16);
						auto extra_asm_args = split_flags_string(temporary_allocator(), build_context.extra_assembler_flags);

						if (build_context.metrics.arch == TargetArch_riscv64) {
							array_add_elems(&asm_args, clang_prefix_args.data, clang_prefix_args.count);
							array_add(&asm_args, asm_file);
							array_add(&asm_args, str_lit("-c"));
							array_add(&asm_args, str_lit("-o"));
							array_add(&asm_args, obj_file);
							array_add(&asm_args, str_lit("-target"));
							array_add(&asm_args, build_context.metrics.target_triplet);
							array_add(&asm_args, str_lit("-march=rv64gc"));
							array_add_elems(&asm_args, extra_asm_args.data, extra_asm_args.count);

							result = run_subprocess(clang_program, slice_from_array(asm_args), true);
						} else if (is_osx) {
							// `as` comes with MacOS.
							array_add(&asm_args, asm_file);
							array_add(&asm_args, str_lit("-o"));
							array_add(&asm_args, obj_file);
							array_add_elems(&asm_args, extra_asm_args.data, extra_asm_args.count);

							result = run_subprocess(str_lit("as"), slice_from_array(asm_args), true);
						} else if (build_context.metrics.arch == TargetArch_arm64) {
							array_add_elems(&asm_args, clang_prefix_args.data, clang_prefix_args.count);
							array_add(&asm_args, asm_file);
							array_add(&asm_args, str_lit("-c"));
							array_add(&asm_args, str_lit("-o"));
							array_add(&asm_args, obj_file);
							array_add(&asm_args, str_lit("-target"));
							array_add(&asm_args, build_context.metrics.target_triplet);
							array_add_elems(&asm_args, extra_asm_args.data, extra_asm_args.count);

							result = run_subprocess(clang_program, slice_from_array(asm_args), true);
						} else {
							// Note(bumbread): I'm assuming nasm is installed on the host machine.
							// Shipping binaries on unix-likes gets into the weird territorry of
							// "which version of glibc" is it linked with.
							array_add(&asm_args, asm_file);
							array_add(&asm_args, str_lit("-f"));
							array_add(&asm_args, obj_format);
							array_add(&asm_args, str_lit("-o"));
							array_add(&asm_args, obj_file);
							array_add_elems(&asm_args, extra_asm_args.data, extra_asm_args.count);

							result = run_subprocess(str_lit("nasm"), slice_from_array(asm_args), true);
							if (result) {
								gb_printf_err("executing `nasm` to assemble foreing import of %.*s failed.\n\tSuggestion: `nasm` does not ship with the compiler and should be installed with your system's package manager.\n", LIT(asm_file));
								return result;
							}
						}
						array_add(&gen->output_object_paths, obj_file);
					} else {
						bool short_circuit = false;
						if (string_ends_with(lib, str_lit(".framework"))) {
							short_circuit = true;
						} else if (string_ends_with(lib, str_lit(".dylib"))) {
							short_circuit = true;
						} else if (string_ends_with(lib, str_lit(".so"))) {
							short_circuit = true;
						} else if (e->LibraryName.ignore_duplicates) {
							short_circuit = true;
						}

						if (string_set_update(&min_libs_set, lib) && (build_context.min_link_libs || short_circuit)) {
							continue;
						}

						if (prev_lib == lib) {
							continue;
						}
						prev_lib = lib;

						// Do not add libc again, this is added later already, and omitted with
						// the `-no-crt` flag, not skipping here would cause duplicate library
						// warnings when linking on darwin and might link libc silently even with `-no-crt`.
						if (lib == str_lit("System.framework") || lib == str_lit("System") || lib == str_lit("c")) {
							continue;
						}

						if (build_context.metrics.os == TargetOs_darwin) {
							if (string_ends_with(lib, str_lit(".framework"))) {
								// framework thingie
								String lib_name = lib;
								lib_name = remove_extension_from_path(lib_name);
								array_add(&lib_args, str_lit("-framework"));
								array_add(&lib_args, lib_name);
							} else if (string_ends_with(lib, str_lit(".a")) || string_ends_with(lib, str_lit(".o")) || string_ends_with(lib, str_lit(".dylib"))) {
								// For:
								// object
								// dynamic lib
								// static libs, absolute full path relative to the file in which the lib was imported from
								array_add(&lib_args, lib);
							} else {
								// dynamic or static system lib, just link regularly searching system library paths
								add_arg_fmt(&lib_args, "-l%.*s", LIT(lib));
							}
						} else {
							// NOTE(vassvik): static libraries (.a files) in linux can be linked to directly using the full path,
							//                since those are statically linked to at link time. shared libraries (.so) has to be
							//                available at runtime wherever the executable is run, so we make require those to be
							//                local to the executable (unless the system collection is used, in which case we search
							//                the system library paths for the library file).
							if (string_ends_with(lib, str_lit(".a")) || string_ends_with(lib, str_lit(".o")) || string_ends_with(lib, str_lit(".so")) || string_contains_string(lib, str_lit(".so."))) {
								add_arg_fmt(&lib_args, "-l:%.*s", LIT(lib));
							} else {
								// dynamic or static system lib, just link regularly searching system library paths
								add_arg_fmt(&lib_args, "-l%.*s", LIT(lib));
							}
						}
					}
				}
			}

			auto object_files = array_make<String>(temporary_allocator(), 0, 32);


			if (is_android) { // NOTE(bill): glue code needed for Android
				TIME_SECTION("Android Native App Glue Compile");

				String android_glue_object = {};
				String android_glue_static_lib = {};

				char hash_buf[64] = {};
				gb_snprintf(hash_buf, gb_size_of(hash_buf), "%p", &hash_buf);
				String hash = make_string_c(hash_buf);

				String temp_dir = normalize_path(temporary_allocator(), temporary_directory(temporary_allocator()), NIX_SEPARATOR_STRING);
				android_glue_object = concatenate4_strings(temporary_allocator(), temp_dir, str_lit("android_native_app_glue-"), hash, str_lit(".o"));
				android_glue_static_lib = concatenate4_strings(permanent_allocator(), temp_dir, str_lit("libandroid_native_app_glue-"), hash, str_lit(".a"));

				String glue_program = concatenate_strings(temporary_allocator(), ODIN_ANDROID_NDK_TOOLCHAIN, str_lit("bin/clang"));

				auto glue = array_make<String>(temporary_allocator(), 0, 8);
				add_arg_fmt(&glue, "--target=%.*s%d", LIT(build_context.metrics.target_triplet), ODIN_ANDROID_API_LEVEL);
				array_add(&glue, str_lit("-c"));
				add_arg_fmt(&glue, "%.*ssources/android/native_app_glue/android_native_app_glue.c", LIT(ODIN_ANDROID_NDK));
				array_add(&glue, str_lit("-o"));
				array_add(&glue, android_glue_object);

				array_add(&glue, str_lit("--sysroot"));
				add_arg_fmt(&glue, "%.*ssysroot", LIT(ODIN_ANDROID_NDK_TOOLCHAIN));

				add_arg_fmt(&glue, "-I%.*ssysroot/usr/include/", LIT(ODIN_ANDROID_NDK_TOOLCHAIN));
				add_arg_fmt(&glue, "-I%.*ssysroot/usr/include/%.*s/", LIT(ODIN_ANDROID_NDK_TOOLCHAIN), LIT(ODIN_ANDROID_NDK_TOOLCHAIN_LIB));

				array_add(&glue, str_lit("-Wno-macro-redefined"));

				result = run_subprocess(glue_program, slice_from_array(glue), false);
				if (result) {
					return result;
				}

				TIME_SECTION("Android Native App Glue ar");

				String ar_program = concatenate_strings(temporary_allocator(), ODIN_ANDROID_NDK_TOOLCHAIN, str_lit("bin/llvm-ar"));

				auto ar = array_make<String>(temporary_allocator(), 0, 4);
				array_add(&ar, str_lit("rcs"));
				array_add(&ar, android_glue_static_lib);
				array_add(&ar, android_glue_object);

				result = run_subprocess(ar_program, slice_from_array(ar), false);
				if (result) {
					return result;
				}

				array_add(&object_files, android_glue_static_lib);
			}


			for (String object_path : gen->output_object_paths) {
				array_add(&object_files, object_path);
			}

			auto link_settings = array_make<String>(temporary_allocator(), 0, 16);

			if (build_context.no_crt) {
				array_add(&link_settings, str_lit("-nostdlib"));
			}

			if (build_context.build_mode == BuildMode_StaticLibrary) {
				TIME_SECTION("Static Library Creation");

				auto ar_args = array_make<String>(temporary_allocator(), 0, object_files.count + 2);
				array_add(&ar_args, str_lit("rcs"));
				array_add(&ar_args, output_filename);
				array_add_elems(&ar_args, object_files.data, object_files.count);

				result = run_subprocess(str_lit("ar"), slice_from_array(ar_args), true);
				if (result) {
					return result;
				}

				return result;
			}

			// NOTE(dweiler): We use clang as a frontend for the linker as there are
			// other runtime and compiler support libraries that need to be linked in
			// very specific orders such as libgcc_s, ld-linux-so, unwind, etc.
			// These are not always typically inside /lib, /lib64, or /usr versions
			// of that, e.g libgcc.a is in /usr/lib/gcc/{version}, and can vary on
			// the distribution of Linux even. The gcc or clang specs is the only
			// reliable way to query this information to call ld directly.
			if (build_context.build_mode == BuildMode_DynamicLibrary) {
				// NOTE(dweiler): Let the frontend know we're building a shared library
				// so it doesn't generate symbols which cannot be relocated.
				array_add(&link_settings, str_lit("-shared"));

				// NOTE(dweiler): _odin_entry_point must be called at initialization
				// time of the shared object, similarly, _odin_exit_point must be called
				// at deinitialization. We can pass both -init and -fini to the linker by
				// using a comma separated list of arguments to -Wl.
				//
				// This previously used ld but ld cannot actually build a shared library
				// correctly this way since all the other dependencies provided implicitly
				// by the compiler frontend are still needed and most of the command
				// line arguments prepared previously are incompatible with ld.
				if (build_context.metrics.os == TargetOs_darwin) {
					array_add(&link_settings, str_lit("-Wl,-init,__odin_entry_point"));
					// NOTE(weshardee): __odin_exit_point should also be added, but -fini
					// does not exist on MacOS
				} else {
					array_add(&link_settings, str_lit("-Wl,-init,_odin_entry_point"));
					array_add(&link_settings, str_lit("-Wl,-fini,_odin_exit_point"));
				}
			} else if (is_android) {
				// Always shared even in android!
				array_add(&link_settings, str_lit("-shared"));
			}

			if (build_context.build_mode == BuildMode_Executable && build_context.reloc_mode == RelocMode_PIC) {
				if (build_context.metrics.os == TargetOs_linux) {
					// Linux does not enable PIE by default but required for ASLR
					array_add(&link_settings, str_lit("-pie"));
				} else {
					// Do not disable PIE, let the linker choose. (most likely you want it enabled)
				}
			} else if (build_context.build_mode != BuildMode_DynamicLibrary) {
				if (build_context.metrics.os != TargetOs_openbsd
					&& build_context.metrics.arch != TargetArch_riscv64
					&& !is_android
				) {
					// OpenBSD defaults to PIE executable, do not pass -no-pie for it.
					array_add(&link_settings, str_lit("-no-pie"));
				}
			}

			auto platform_lib_args = array_make<String>(temporary_allocator(), 0, 16);
			if (build_context.metrics.os == TargetOs_darwin) {
				// Get the SDK path.
				gbString darwin_sdk_path = gb_string_make(temporary_allocator(), "");

				char const* darwin_platform_name  = "MacOSX";
				char const* darwin_xcrun_sdk_name = "macosx";
				char const* darwin_min_version_id = "macosx";

				String original_clang_program = clang_program;
				auto original_clang_prefix_args = clang_prefix_args;

				// NOTE(harold): We set the clang_path to run through xcrun because otherwise it complaints about the the sysroot
				//               being set to 'MacOSX' even though we've set the sysroot to the correct SDK (-Wincompatible-sysroot).
				//               This is because it is likely not using the SDK's toolchain Apple Clang but another one installed in the system.
				switch (selected_subtarget) {
				case Subtarget_iPhone:
					darwin_platform_name  = "iPhoneOS";
					darwin_xcrun_sdk_name = "iphoneos";
					darwin_min_version_id = "ios";
					if (!has_odin_clang_path_env) {
						clang_program = str_lit("xcrun");
						clang_prefix_args = array_make<String>(temporary_allocator(), 0, 4);
						array_add(&clang_prefix_args, str_lit("--sdk"));
						array_add(&clang_prefix_args, str_lit("iphoneos"));
						array_add(&clang_prefix_args, str_lit("clang"));
					}
					break;
				case Subtarget_iPhoneSimulator:
					darwin_platform_name  = "iPhoneSimulator";
					darwin_xcrun_sdk_name = "iphonesimulator";
					darwin_min_version_id = "ios-simulator";
					if (!has_odin_clang_path_env) {
						clang_program = str_lit("xcrun");
						clang_prefix_args = array_make<String>(temporary_allocator(), 0, 4);
						array_add(&clang_prefix_args, str_lit("--sdk"));
						array_add(&clang_prefix_args, str_lit("iphonesimulator"));
						array_add(&clang_prefix_args, str_lit("clang"));
					}
					break;
				}

				auto darwin_find_sdk_args = array_make<String>(temporary_allocator(), 0, 4);
				array_add(&darwin_find_sdk_args, str_lit("--sdk"));
				array_add(&darwin_find_sdk_args, make_string_c(darwin_xcrun_sdk_name));
				array_add(&darwin_find_sdk_args, str_lit("--show-sdk-path"));

				if (capture_subprocess(str_lit("xcrun"), slice_from_array(darwin_find_sdk_args), true, &darwin_sdk_path) != 0) {

					// Fallback to default clang, since `xcrun --sdk` did not work.
					clang_program = original_clang_program;
					clang_prefix_args = original_clang_prefix_args;

					// Best-effort fallback to known locations
					gbString darwin_sdk_path = gb_string_make(temporary_allocator(), "");
					darwin_sdk_path = gb_string_append_fmt(darwin_sdk_path, "/Library/Developer/CommandLineTools/SDKs/%s.sdk", darwin_platform_name);

					if (!path_is_directory(make_string_c(darwin_sdk_path))) {
						gb_string_clear(darwin_sdk_path);
						darwin_sdk_path = gb_string_append_fmt(darwin_sdk_path, "/Applications/Xcode.app/Contents/Developer/Platforms/%s.platform/Developer/SDKs/%s.sdk", darwin_platform_name);

						if (!path_is_directory(make_string_c(darwin_sdk_path))) {
							gb_printf_err("Failed to find %s SDK\n", darwin_platform_name);
							return -1;
						}
					}
				} else {
					// Trim the trailing newline.
					darwin_sdk_path = gb_string_trim_space(darwin_sdk_path);
				}
				array_add(&platform_lib_args, str_lit("--sysroot"));
				array_add(&platform_lib_args, make_string_c(darwin_sdk_path));

				array_add(&platform_lib_args, str_lit("-L/usr/local/lib"));

				// Homebrew's default library path, checking if it exists to avoid linking warnings.
				if (gb_file_exists("/opt/homebrew/lib")) {
					array_add(&platform_lib_args, str_lit("-L/opt/homebrew/lib"));
				}

				// MacPort's default library path, checking if it exists to avoid linking warnings.
				if (gb_file_exists("/opt/local/lib")) {
					array_add(&platform_lib_args, str_lit("-L/opt/local/lib"));
				}

				// Only specify this flag if the user has given a minimum version to target.
				// This will cause warnings to show up for mismatched libraries.
				// NOTE(harold): For device subtargets we have to explicitly set the default version to
				//               avoid the same warning since we configure our own minimum version when compiling for devices.
				if (build_context.minimum_os_version_string_given || selected_subtarget != Subtarget_Default) {
					add_arg_fmt(&link_settings, "-m%s-version-min=%.*s", darwin_min_version_id, LIT(build_context.minimum_os_version_string));
				}

				if (build_context.build_mode != BuildMode_DynamicLibrary) {
					// This points the linker to where the entry point is
					array_add(&link_settings, str_lit("-e"));
					array_add(&link_settings, str_lit("_main"));
				}
			} else if (build_context.metrics.os == TargetOs_freebsd) {
				if (build_context.sanitizer_flags & (SanitizerFlag_Address | SanitizerFlag_Memory)) {
					// It's imperative that `pthread` is linked before `libc`,
					// otherwise ASan/MSan will be unable to call `pthread_key_create`
					// because FreeBSD's `libthr` implementation of `pthread`
					// needs to replace the relevant stubs first.
					//
					// (Presumably TSan implements its own `pthread` interface,
					//  which is why it isn't required.)
					//
					// See: https://reviews.llvm.org/D39254
					array_add(&platform_lib_args, str_lit("-lpthread"));
				}
				// FreeBSD pkg installs third-party shared libraries in /usr/local/lib.
				array_add(&platform_lib_args, str_lit("-Wl,-L/usr/local/lib"));
			} else if (build_context.metrics.os == TargetOs_openbsd) {
				// OpenBSD ports install shared libraries in /usr/local/lib. Also, we must explicitly link libpthread.
				array_add(&platform_lib_args, str_lit("-lpthread"));
				array_add(&platform_lib_args, str_lit("-Wl,-L/usr/local/lib"));
				// Until the LLVM back-end can be adapted to emit endbr64 instructions on amd64, we
				// need to pass -z nobtcfi in order to allow the resulting program to run under
				// OpenBSD 7.4 and newer. Once support is added at compile time, this can be dropped.
				array_add(&platform_lib_args, str_lit("-Wl,-z,nobtcfi"));
			} else if (build_context.metrics.os == TargetOs_linux) {
				// required for RELRO
				array_add(&platform_lib_args, str_lit("-Wl,-z,now"));
				array_add(&platform_lib_args, str_lit("-Wl,-z,relro"));
			}

			if (is_android) {
				GB_ASSERT(ODIN_ANDROID_NDK_TOOLCHAIN_LIB.len != 0);
				GB_ASSERT(ODIN_ANDROID_NDK_TOOLCHAIN_LIB_LEVEL.len != 0);
				GB_ASSERT(ODIN_ANDROID_NDK_TOOLCHAIN_SYSROOT.len != 0);

				add_arg_fmt(&platform_lib_args, "-L%.*susr/lib/%.*s/%d",
					LIT(ODIN_ANDROID_NDK_TOOLCHAIN_SYSROOT), LIT(ODIN_ANDROID_NDK_TOOLCHAIN_LIB), ODIN_ANDROID_API_LEVEL);

				array_add(&platform_lib_args, str_lit("-landroid"));
				array_add(&platform_lib_args, str_lit("-llog"));

				add_arg_fmt(&platform_lib_args, "--sysroot=%.*s", LIT(ODIN_ANDROID_NDK_TOOLCHAIN_SYSROOT));

				array_add(&link_settings, str_lit("-u"));
				array_add(&link_settings, str_lit("ANativeActivity_onCreate"));
			}

			if (!build_context.no_rpath) {
				// Set the rpath to the $ORIGIN/@loader_path (the path of the executable),
				// so that dynamic libraries are looked for at that path.
				if (build_context.metrics.os == TargetOs_darwin) {
					array_add(&link_settings, str_lit("-Wl,-rpath,@loader_path"));
				} else {
					if (is_android) {
						// ignore
					} else {
						array_add(&link_settings, str_lit("-Wl,-rpath,$ORIGIN"));
					}
				}
			}

			if (!build_context.no_crt) {
				array_add(&lib_args, str_lit("-lm"));
				if (build_context.metrics.os == TargetOs_darwin) {
					// NOTE: adding this causes a warning about duplicate libraries, I think it is
					// automatically assumed/added by clang when you don't do `-nostdlib`.
					// array_add(&lib_args, str_lit("-lSystem"));
				} else {
					array_add(&lib_args, str_lit("-lc"));
				}
			}

			String link_program = {};
			auto link_args = array_make<String>(temporary_allocator(), 0, 64);

			if (is_android) {
				link_program = concatenate_strings(temporary_allocator(), ODIN_ANDROID_NDK_TOOLCHAIN, str_lit("bin/clang"));
				add_arg_fmt(&link_args, "--target=%.*s%d", LIT(build_context.metrics.target_triplet), ODIN_ANDROID_API_LEVEL);
			} else {
				link_program = clang_program;
				array_add_elems(&link_args, clang_prefix_args.data, clang_prefix_args.count);
			}
			array_add(&link_args, str_lit("-Wno-unused-command-line-argument"));

			if (build_context.lto_kind != LTO_None) {
				array_add(&link_args, str_lit("-flto=thin"));
				add_arg_fmt(&link_args, "-flto-jobs=%d", build_context.thread_count);

				if (build_context.ODIN_DEBUG) {
					array_add(&link_args, str_lit("-g"));
				}

				if (is_osx && !build_context.minimum_os_version_string_given) {
					array_add(&link_args, str_lit("-Wno-override-module"));
				}
			}

			array_add_elems(&link_args, object_files.data, object_files.count);
			array_add(&link_args, str_lit("-o"));
			array_add(&link_args, output_filename);
			array_add_elems(&link_args, platform_lib_args.data, platform_lib_args.count);
			array_add_elems(&link_args, lib_args.data, lib_args.count);
			array_add_elems(&link_args, build_context.link_flags.data, build_context.link_flags.count);
			for (String const &flag : split_flags_string(temporary_allocator(), build_context.extra_linker_flags)) {
				array_add(&link_args, flag);
			}
			array_add_elems(&link_args, link_settings.data, link_settings.count);


			if (is_android) {
				TIME_SECTION("Linking");
			}

			if (build_context.linker_choice == Linker_lld) {
				array_add(&link_args, str_lit("-fuse-ld=lld"));
			} else if (build_context.linker_choice == Linker_mold) {
				array_add(&link_args, str_lit("-fuse-ld=mold"));
			}
			result = run_subprocess(link_program, slice_from_array(link_args), true);

			if (result) {
				return result;
			}

			if (is_osx && build_context.ODIN_DEBUG) {
				// NOTE: macOS links DWARF symbols dynamically. Dsymutil will map the stubs in the exe
				// to the symbols in the object file
				auto dsymutil_args = array_make<String>(temporary_allocator(), 0, 1);
				array_add(&dsymutil_args, output_filename);

				result = run_subprocess(str_lit("dsymutil"), slice_from_array(dsymutil_args), true);

				if (result) {
					return result;
				}
			}
		}
	}

	return result;
}
