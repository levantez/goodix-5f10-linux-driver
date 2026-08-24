project('goodixtls5f10-tod', ['c', 'cpp'],
        version : '0.1',
        default_options : ['c_std=gnu11', 'cpp_std=c++17',
                           'warning_level=1', 'werror=false',
                           'b_lundef=false'])  # fp_* резолвит хост (fprintd) в рантайме

deps = [
  dependency('libfprint-2-tod-1'),
  dependency('openssl'),
  dependency('gusb'),
  dependency('glib-2.0'),
  dependency('gio-2.0'),
  dependency('gmodule-2.0'),
  dependency('opencv4'),
]

srcs = files(
  'src/goodix.c',
  'src/goodix_proto.c',
  'src/goodixtls.c',
  'src/goodix5f10.c',
  'src/sigfm/sigfm.cpp',
)

shared_library('fprint-tod-goodix5f10',
               srcs,
               dependencies : deps,
               include_directories : include_directories('src'),
               name_prefix : 'lib',
               install : false)
