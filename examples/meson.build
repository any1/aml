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

libuv = dependency('libuv', required: false)

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
