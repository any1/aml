libuv = dependency('libuv', required: false)
libwayland_server = dependency('wayland-server', required: false)

executable(
	'ticker',
	[
		'ticker.c',
	],
	dependencies: [
		aml_dep,
		threads,
	]
)

executable(
	'reader',
	[
		'reader.c',
	],
	dependencies: [
		aml_dep,
		threads,
	]
)

if libuv.found()
	executable(
		'uv',
		[
			'uv.c',
		],
		dependencies: [
			aml_dep,
			threads,
			libuv,
		]
	)
endif

if libwayland_server.found()
	executable(
		'wayland',
		[
			'wayland.c',
		],
		dependencies: [
			aml_dep,
			threads,
			libwayland_server,
		]
	)
endif
