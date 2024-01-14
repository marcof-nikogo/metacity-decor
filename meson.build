project(
	'metacity_decor',
	'c',
	'cpp',
	version: '0.1',
	license: 'MIT',
	meson_version: '>=0.51.0',
	default_options: [
		'cpp_std=c++17',
        'c_std=c99',
		'warning_level=2',
		'werror=false',
	],
)

add_project_arguments(['-DWLR_USE_UNSTABLE'], language: ['cpp', 'c'])
add_project_arguments(['-DWAYFIRE_PLUGIN'], language: ['cpp', 'c'])
add_project_link_arguments(['-rdynamic'], language:'cpp')
add_global_arguments(['-Wno-unused','-Wno-unused-parameter'],language: 'c')
add_global_arguments(['-Wno-unused','-Wno-unused-parameter'],language: 'cpp')

wayfire        = dependency('wayfire')
wfconfig       = dependency('wf-config')
pixman         = dependency('pixman-1')
glib           = dependency('glib-2.0')
gtk3           = dependency('gtk+-3.0')
gdk_pixbuf     = dependency('gdk-pixbuf-2.0')
wlroots        = dependency('wlroots')
cairo          = dependency('cairo')
pango          = dependency('pango')
pangocairo     = dependency('pangocairo')

subdir('src')
subdir('metadata')
subdir('assets')


