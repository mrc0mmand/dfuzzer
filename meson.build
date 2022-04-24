project('dfuzzer', 'c',
        version : '1.4',
        default_options: [
                'c_std=gnu11',
                'prefix=/usr',
                'warning_level=2',
        ],
)

libgio = dependency('gio-2.0', required : true)
libffi = dependency('libffi', required : true)

subdir('src')

# https://github.com/matusmarhefka/dfuzzer/issues/30
add_project_arguments('-Wno-unused-parameter', language : 'c')

executable(
        'dfuzzer',
        dfuzzer_sources,
        dependencies : [libgio, libffi],
        install : true
)

install_data('src/dfuzzer.conf', install_dir : get_option('sysconfdir'))
install_man('man/dfuzzer.1')