i32 bundle_android(String init_directory);

i32 bundle(String init_directory) {
	switch (build_context.command_kind) {
	case Command_bundle_android:
		return bundle_android(init_directory);
	}
	gb_printf_err("Unknown odin package <platform>\n");
	return 1;
}


i32 bundle_android(String original_init_directory) {
	TEMPORARY_ALLOCATOR_GUARD();

	i32 result = 0;
	init_android_values(/*with_sdk*/true);

	bool init_directory_ok = false;
	String init_directory = path_to_fullpath(temporary_allocator(), original_init_directory, &init_directory_ok);
	if (!init_directory_ok) {
		gb_printf_err("Error: '%.*s' is not a valid directory", LIT(original_init_directory));
		return 1;
	}
	init_directory = normalize_path(temporary_allocator(), init_directory, NIX_SEPARATOR_STRING);

	int const ODIN_ANDROID_API_LEVEL = build_context.ODIN_ANDROID_API_LEVEL;

	String android_sdk_build_tools = concatenate3_strings(temporary_allocator(),
		build_context.ODIN_ANDROID_SDK, str_lit("build-tools"), NIX_SEPARATOR_STRING);

	Array<FileInfo> list = {};
	ReadDirectoryError rd_err = read_directory(android_sdk_build_tools, &list);
	defer (array_free(&list));

	switch (rd_err) {
	case ReadDirectory_InvalidPath:
		gb_printf_err("Invalid path: %.*s\n", LIT(android_sdk_build_tools));
		return 1;
	case ReadDirectory_NotExists:
		gb_printf_err("Path does not exist: %.*s\n", LIT(android_sdk_build_tools));
		return 1;
	case ReadDirectory_Permission:
		gb_printf_err("Unknown error whilst reading path %.*s\n", LIT(android_sdk_build_tools));
		return 1;
	case ReadDirectory_NotDir:
		gb_printf_err("Expected a directory for a package, got a file: %.*s\n", LIT(android_sdk_build_tools));
		return 1;
	case ReadDirectory_Empty:
		gb_printf_err("Empty directory: %.*s\n", LIT(android_sdk_build_tools));
		return 1;
	case ReadDirectory_Unknown:
		gb_printf_err("Unknown error whilst reading path %.*s\n", LIT(android_sdk_build_tools));
		return 1;
	}

	auto possible_valid_dirs = array_make<FileInfo>(heap_allocator(), 0, list.count);
	defer (array_free(&possible_valid_dirs));


	for (FileInfo fi : list) if (fi.is_dir) {
		bool all_numbers = true;
		for (isize i = 0; i < fi.name.len; i++) {
			u8 c = fi.name[i];
			if ('0' <= c && c <= '9') {
				// true
			} else if (i == 0) {
				all_numbers = false;
			} else if (c == '.') {
				break;
			} else {
				all_numbers = false;
			}
		}

		if (all_numbers) {
			array_add(&possible_valid_dirs, fi);
		}
	}

	if (possible_valid_dirs.count == 0) {
		gb_printf_err("Unable to find any Android SDK/API Level in %.*s\n", LIT(android_sdk_build_tools));
		return 1;
	}

	int *dir_numbers = temporary_alloc_array<int>(possible_valid_dirs.count);

	char buf[1024] = {};
	for_array(i, possible_valid_dirs) {
		FileInfo fi = possible_valid_dirs[i];
		isize n = gb_min(gb_size_of(buf)-1, fi.name.len);
		memcpy(buf, fi.name.text, n);
		buf[n] = 0;

		dir_numbers[i] = atoi(buf);
	}

	isize closest_number_idx = -1;
	for (isize i = 0; i < possible_valid_dirs.count; i++) {
		if (dir_numbers[i] >= ODIN_ANDROID_API_LEVEL) {
			if (closest_number_idx < 0) {
				closest_number_idx = i;
			} else if (dir_numbers[i] < dir_numbers[closest_number_idx]) {
				closest_number_idx = i;
			}
		}
	}

	if (closest_number_idx < 0) {
		gb_printf_err("Unable to find any Android SDK/API Level in %.*s meeting the minimum API level of %d\n", LIT(android_sdk_build_tools), ODIN_ANDROID_API_LEVEL);
		return 1;
	}

	String api_number = possible_valid_dirs[closest_number_idx].name;

	android_sdk_build_tools = concatenate_strings(temporary_allocator(), android_sdk_build_tools, api_number);
	String android_sdk_platforms = concatenate_strings(temporary_allocator(),
		build_context.ODIN_ANDROID_SDK,
		make_string_c(gb_bprintf("platforms/android-%d/", dir_numbers[closest_number_idx]))
	);

	android_sdk_build_tools = normalize_path(temporary_allocator(), android_sdk_build_tools, NIX_SEPARATOR_STRING);
	android_sdk_platforms   = normalize_path(temporary_allocator(), android_sdk_platforms,   NIX_SEPARATOR_STRING);

	String output_filename = str_lit("test");
	String output_apk = path_remove_extension(output_filename);

	TIME_SECTION("Android aapt");
	{
		TEMPORARY_ALLOCATOR_GUARD();

		String manifest = concatenate_strings(temporary_allocator(), init_directory, str_lit("AndroidManifest.xml"));

		String program = concatenate_strings(temporary_allocator(), android_sdk_build_tools, str_lit("aapt"));

		auto args = array_make<String>(temporary_allocator(), 0, 16);
		array_add(&args, str_lit("package"));
		array_add(&args, str_lit("-f"));
		array_add(&args, str_lit("-M"));
		array_add(&args, manifest);
		array_add(&args, str_lit("-I"));
		add_arg_fmt(&args, "%.*sandroid.jar", LIT(android_sdk_platforms));
		array_add(&args, str_lit("-F"));
		add_arg_fmt(&args, "%.*s.apk-build", LIT(output_apk));

		String resources_dir = concatenate_strings(temporary_allocator(), init_directory, str_lit("res"));
		if (gb_file_exists((const char *)resources_dir.text)) {
			array_add(&args, str_lit("-S"));
			array_add(&args, resources_dir);
		}

		String assets_dir = concatenate_strings(temporary_allocator(), init_directory, str_lit("assets"));
		if (gb_file_exists((const char *)assets_dir.text)) {
			array_add(&args, str_lit("-A"));
			array_add(&args, assets_dir);
		}

		String lib_dir = concatenate_strings(temporary_allocator(), init_directory, str_lit("lib"));
		if (gb_file_exists((const char *)lib_dir.text)) {
			array_add(&args, lib_dir);
		}

		result = run_subprocess(program, slice_from_array(args), false);
		if (result) {
			return result;
		}
	}

	TIME_SECTION("Android zipalign");
	{
		TEMPORARY_ALLOCATOR_GUARD();

		String program = concatenate_strings(temporary_allocator(), android_sdk_build_tools, str_lit("zipalign"));

		auto args = array_make<String>(temporary_allocator(), 0, 8);
		array_add(&args, str_lit("-f"));
		array_add(&args, str_lit("4"));
		add_arg_fmt(&args, "%.*s.apk-build", LIT(output_apk));
		add_arg_fmt(&args, "%.*s.apk", LIT(output_apk));

		result = run_subprocess(program, slice_from_array(args), false);
		if (result) {
			return result;
		}
	}

	TIME_SECTION("Android apksigner");
	{
		TEMPORARY_ALLOCATOR_GUARD();

#if defined(GB_SYSTEM_WINDOWS)
		String apksigner = str_lit("apksigner.bat");
#else
		String apksigner = str_lit("apksigner");
#endif
		String program = concatenate_strings(temporary_allocator(), android_sdk_build_tools, apksigner);

		auto args = array_make<String>(temporary_allocator(), 0, 16);
		array_add(&args, str_lit("sign"));

		String keystore = normalize_path(temporary_allocator(), build_context.android_keystore, NIX_SEPARATOR_STRING);
		keystore = substring(keystore, 0, keystore.len - 1);
		array_add(&args, str_lit("--ks"));
		array_add(&args, keystore);

		if (build_context.android_keystore_alias.len != 0) {
			array_add(&args, str_lit("--ks-key-alias"));
			array_add(&args, build_context.android_keystore_alias);
		}
		if (build_context.android_keystore_password.len != 0) {
			array_add(&args, str_lit("--ks-pass"));
			add_arg_fmt(&args, "pass:%.*s", LIT(build_context.android_keystore_password));
		}

		add_arg_fmt(&args, "%.*s.apk", LIT(output_apk));

#if defined(GB_SYSTEM_WINDOWS)
		auto bat_args = array_make<String>(temporary_allocator(), 0, args.count + 2);
		array_add(&bat_args, str_lit("/c"));
		array_add(&bat_args, program);
		for (String const &arg : args) {
			array_add(&bat_args, arg);
		}
		result = run_subprocess(str_lit("cmd.exe"), slice_from_array(bat_args), true);
#else
		result = run_subprocess(program, slice_from_array(args), false);
#endif
		if (result) {
			return result;
		}
	}

	return 0;
}
