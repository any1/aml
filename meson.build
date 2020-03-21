project(
	'aml',
	'c',
	version: '0.0.0',
	license: 'ISC',
	default_options: [
		'c_std=c11',
	]
)

buildtype = get_option('buildtype')

c_args = [
	'-D_POSIX_C_SOURCE=200809L',
	'-fvisibility=hidden',
]

if buildtype != 'debug' and buildtype != 'debugoptimized'
	c_args += '-DNDEBUG'
endif

add_project_arguments(c_args, language: 'c')

cc = meson.get_compiler('c')

librt = cc.find_library('rt', required: true)
threads = dependency('threads')

inc = include_directories('include')

sources = [
	'src/aml.c',
	'src/posix.c',
	'src/thread-pool.c',
]

dependencies = [
	librt,
	threads,
]

aml = shared_library(
	'aml',
	sources,
	version: '0.0.0',
	dependencies: dependencies,
	include_directories: inc,
	install: true,
)

aml_dep = declare_dependency(
	include_directories: inc,
	link_with: aml,
)

if get_option('examples')
	subdir('examples')
endif

install_headers('include/aml.h')

pkgconfig = import('pkgconfig')
pkgconfig.generate(
	libraries: aml,
	version: meson.project_version(),
	filebase: meson.project_name(),
	name: meson.project_name(),
	description: 'Amother main loop library',
)
