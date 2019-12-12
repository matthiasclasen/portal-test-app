project('portal-test-app','c',
        version: '0.1',
        meson_version: '>= 0.46.0')

gio_dep = dependency('gio-unix-2.0')
libportal_dep = dependency('libportal')

src = ['main.c']

executable('portal-test',
           src,
           dependencies: [gio_dep, libportal_dep],
           install : true)
