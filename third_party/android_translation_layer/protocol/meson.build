fs = import('fs')

proto_inc = include_directories('.')

wl_protocol_dir = wayland_protos_dep.get_variable(pkgconfig: 'pkgdatadir')

wayland_scanner = find_program('wayland-scanner')

wl_protos = [
  ['input-method-unstable-v2.xml'],
  ['virtual-keyboard-unstable-v1.xml'],
]

wl_proto_sources = []
wl_proto_headers = []

foreach p : wl_protos
  xml = join_paths(p)

  base = fs.name(xml)
  proto = fs.stem(base)

  wl_proto_headers += custom_target('@0@ client header'.format(proto),
				    input: xml,
				    output: '@0@-client-protocol.h'.format(proto),
				    command: [wayland_scanner,
					      'client-header',
					      '@INPUT@',
					      '@OUTPUT@']
				   )
  wl_proto_sources += custom_target('@0@ source'.format(proto),
				    input: xml,
				    output: '@0@-protocol.c'.format(proto),
				    command: [wayland_scanner,
					      'private-code',
					      '@INPUT@',
					      '@OUTPUT@']
				   )
endforeach
