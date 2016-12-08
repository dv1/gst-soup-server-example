#!/usr/bin/env python


from waflib.Build import BuildContext, CleanContext, InstallContext, UninstallContext, Logs

top = '.'
out = 'build'


# the code inside fragment deliberately does an unsafe implicit cast float->char to trigger a
# compiler warning; sometimes, gcc does not tell about an unsupported parameter *unless* the
# code being compiled causes a warning
c_cflag_check_code = """
int main()
{
	float f = 4.0;
	char c = f;
	return c - 4;
}
"""
def check_compiler_flag(conf, flag, lang):
	return conf.check(fragment = c_cflag_check_code, mandatory = 0, execute = 0, define_ret = 0, msg = 'Checking for compiler switch %s' % flag, cxxflags = conf.env[lang + 'FLAGS'] + [flag], okmsg = 'yes', errmsg = 'no')  
def check_compiler_flags_2(conf, cflags, ldflags, msg):
	Logs.pprint('NORMAL', msg)
	return conf.check(fragment = c_cflag_check_code, mandatory = 0, execute = 0, define_ret = 0, msg = 'Checking if building with these flags works', cxxflags = cflags, ldflags = ldflags, okmsg = 'yes', errmsg = 'no')


def add_compiler_flags(conf, env, flags, lang, compiler, uselib = ''):
	for flag in reversed(flags):
		if type(flag) == type(()):
			flag_candidate = flag[0]
			flag_alternative = flag[1]
		else:
			flag_candidate = flag
			flag_alternative = None

		if uselib:
			flags_pattern = lang + 'FLAGS_' + uselib
		else:
			flags_pattern = lang + 'FLAGS'

		if check_compiler_flag(conf, flag_candidate, compiler):
			env.prepend_value(flags_pattern, [flag_candidate])
		elif flag_alternative:
			if check_compiler_flag(conf, flag_alternative, compiler):
				env.prepend_value(flags_pattern, [flag_alternative])


def options(opt):
	opt.add_option('--enable-debug', action = 'store_true', default = False, help = 'enable debug build')
	opt.add_option('--enable-test-build', action = 'store_true', default = False, help = 'enable test build')
	opt.load('compiler_cxx')


def configure(conf):
	conf.load('compiler_cxx')

	if conf.env['CXXFLAGS']:
		check_compiler_flags_2(conf, conf.env['CXXFLAGS'], '', "Testing compiler flags %s" % ' '.join(conf.env['CXXFLAGS']))
	if conf.env['LINKFLAGS']:
		check_compiler_flags_2(conf, '', conf.env['LINKFLAGS'], "Testing linker flags %s" % ' '.join(conf.env['LINKFLAGS']))

	compiler_flags = []
	if conf.options.enable_debug:
		compiler_flags += ['-O0', '-g3', '-ggdb']
	else:
		compiler_flags += ['-O2']
	add_compiler_flags(conf, conf.env, compiler_flags + ['-Wextra', '-Wall', '-Wno-variadic-macros', '-std=c++11', '-pedantic'], 'CXX', 'CXX')

	conf.check_cfg(package = 'glib-2.0 >= 2.32.0', uselib_store = 'GLIB', args = '--cflags --libs', mandatory = 1)

	conf.check_cfg(package = 'gstreamer-1.0 >= 1.0.0', uselib_store = 'GSTREAMER', args = '--cflags --libs', mandatory = 1)
	conf.check_cfg(package = 'gstreamer-base-1.0 >= 1.0.0', uselib_store = 'GSTREAMER', args = '--cflags --libs', mandatory = 1)

	conf.check_cfg(package = 'libsoup-2.4 >= 2.25.92', uselib_store = 'SOUP', args = '--cflags --libs', mandatory = 1)


def build(bld):
	bld(
		features = ['cxx', 'cxxprogram'],
		uselib = ['GLIB', 'GSTREAMER', 'SOUP'],
		target = 'gst-soup-server-example',
		source = ['gst-soup-server-example.cpp']
	)
