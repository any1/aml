project(
	'aml',
	'c',
	version: '0.2.0',
	license: 'ISC',
	default_options: [
		'c_std=c11',
	]
)

buildtype = get_option('buildtype')
default_library = get_option('default_library')
is_static_subproject = meson.is_subproject() and default_library == 'static'

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

librt = cc.find_library('rt', required: true)
threads = dependency('threads')

inc = include_directories('include')

sources = [
	'src/aml.c',
	'src/thread-pool.c',
]

have_epoll = cc.has_header_symbol('sys/epoll.h', 'epoll_create')
have_kqueue = cc.has_header_symbol('sys/event.h', 'kqueue')

if have_epoll
	sources += 'src/epoll.c'
	message('epoll backend chosen')
elif have_kqueue
	sources += 'src/kqueue.c'
	message('kqueue backend chosen')
else
	error('Unsupported system')
endif

dependencies = [
	librt,
	threads,
]

aml = library(
	'aml',
	sources,
	version: '0.0.0',
	dependencies: dependencies,
	include_directories: inc,
	install: not is_static_subproject,
)

aml_dep = declare_dependency(
	include_directories: inc,
	link_with: aml,
)

if get_option('examples')
	subdir('examples')
endif

if not is_static_subproject
	install_headers('include/aml.h')

	pkgconfig = import('pkgconfig')
	pkgconfig.generate(
		aml,
		version: meson.project_version(),
		filebase: meson.project_name(),
		name: meson.project_name(),
		description: 'Another main loop library',
	)
endif
