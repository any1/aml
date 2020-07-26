project(
	'aml',
	'c',
	version: '0.1.0',
	license: 'ISC',
	default_options: [
		'c_std=c11',
	]
)

buildtype = get_option('buildtype')

c_args = [
	'-DPROJECT_VERSION="@0@"'.format(meson.project_version()),
	'-D_POSIX_C_SOURCE=200809L',
	'-fvisibility=hidden',
	'-Wmissing-prototypes',
]

git = find_program('git', native: true, required: false)
if git.found()
	git_describe = run_command([git, 'describe', '--tags', '--long'])
	git_branch = run_command([git, 'rev-parse', '--abbrev-ref', 'HEAD'])
	if git_describe.returncode() == 0 and git_branch.returncode() == 0
		c_args += '-DGIT_VERSION="@0@ (@1@)"'.format(
			git_describe.stdout().strip(),
			git_branch.stdout().strip(),
		)
	endif
endif

if buildtype != 'debug' and buildtype != 'debugoptimized'
	c_args += '-DNDEBUG'
endif

add_project_arguments(c_args, language: 'c')

cc = meson.get_compiler('c')

libepoll = dependency('epoll-shim', required: false)
librt = cc.find_library('rt', required: true)
threads = dependency('threads')

inc = include_directories('include')

sources = [
	'src/aml.c',
	'src/epoll.c',
	'src/thread-pool.c',
]

dependencies = [
	libepoll,
	librt,
	threads,
]

aml = library(
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
	aml,
	version: meson.project_version(),
	filebase: meson.project_name(),
	name: meson.project_name(),
	description: 'Amother main loop library',
)
