#!/usr/bin/env python3

import os, sys, subprocess
from collections import OrderedDict
from glob import glob
from ninja_syntax import Writer

GETTEXT_TARBALL = 'gettext-0.20.2.tar.gz'
GETTEXT_SHA256 = 'ecb9d0908ca41d5ca5fef974323b3bba6bec19eebba0b44f396de98cfcc089f1'

_exclusion_list = [
    '.DS_Store',
    '.git',
    'autom4te.cache', 'build_windows',
    'Debug', 'Debug_static', 'Release', 'Release_static',
    'bin',
    'docs', 'doc', 'examples', 'test', 'tests', 'test-driver',
]
_exclusion_gitignore = subprocess.run(['git', 'ls-files', '--others', '-i', '--exclude-standard'],
                                      stdout=subprocess.PIPE).stdout.decode('utf-8').splitlines()

def _is_excluded(e):
    if e.name in _exclusion_list or e.path in _exclusion_gitignore:
        return True
    if e.name.endswith('-tests'):
        return True
    return False

def collect_files(dirname):
    for e in os.scandir(dirname):
        if _is_excluded(e):
            continue
        if e.is_file():
            yield e.path
        elif e.is_dir():
            yield from collect_files(e.path)

def emit_commands(commands):
    for cmd in commands:
        assert "'" not in cmd
        yield "echo 'note: ⋯  %s'" % cmd
        yield 'tmp=`mktemp`'
        yield '%s >$$tmp 2>&1 || (cat $$tmp ; exit 1)' % cmd
        yield 'rm -f $$tmp'

def gen_pre_build_commands(tarball, patches, srcdir):
    yield 'rm -rf "$workdir" "$destdir"'
    yield 'mkdir -p "$workdir"'
    if tarball:
        yield 'tar -x -f "$top_srcdir/%s" -C "$workdir" --strip-components 1' % tarball
        for p in patches:
            yield 'patch -d "$workdir" -p1 < "$top_srcdir/%s"' % p
    else:
        yield 'cp -aR "$srcdir/" "$workdir"'
    yield 'cd "$workdir"'

post_build_commands = [
    'rm -rf "$workdir"',
    'touch $intdir/$name.done',
    ]

default_build_commands = [
    '$configure $configure_flags',
    'make $makeflags',
    'make install -j1 DESTDIR="$destdir"',
    ]


def gen_configure(n, prj, tarball=None, patches=[], srcdir=None, configure='configure', flags=[], build_commands=None):
    target = '$intdir/%s.done' % prj
    if not tarball and not srcdir:
        srcdir = prj
    commands = build_commands if build_commands else default_build_commands
    all_flags = ' '.join(flags)

    configure_commands = list(gen_pre_build_commands(tarball, patches, srcdir)) + commands + post_build_commands
    n.rule('%s_build' % prj,
           description='Building deps/$name...',
           pool='console',
           command=' && '.join(emit_commands(configure_commands)))
    n.build([target],
            '%s_build' % prj,
            inputs=sorted([tarball] + patches if tarball else collect_files(srcdir)),
            variables=OrderedDict([
                ('name', prj),
                ('configure', configure),
                ('configure_flags', all_flags),
                ('srcdir', '$top_srcdir/%s' % (srcdir if srcdir else prj)),
                ('destdir', '$builddir/%s' % prj),
                ('workdir', '$intdir/%s' % prj),
            ]))
    n.build([prj], 'phony', target)
    return target


with open('build.ninja', 'w') as buildfile:
    n = Writer(buildfile, width=20)
    n.comment('generated by %s' % sys.argv[0])
    n.include('build.vars.ninja')

    n.rule('download',
           description='Downloading $url...',
           pool='console',
           command='curl -o $out $url && test "$sha256" = `shasum -a256 $out | cut -f1 -d" "`')

    targets = []

    n.build(['tarballs/%s' % GETTEXT_TARBALL],
            'download',
            variables={
                'url': 'https://ftp.gnu.org/pub/gnu/gettext/%s' % GETTEXT_TARBALL,
                'sha256': GETTEXT_SHA256,
            })

    targets.append(gen_configure(n, 'gettext',
                                 tarball='tarballs/%s' % GETTEXT_TARBALL,
                                 patches=glob('gettext/*.patch'),
                                 configure='./configure',
                                 flags=[
                                     '--prefix=/',
                                     'CC=$cc',
                                     'CXX=$cxx',
                                     # When running configure under Xcode,
                                     # SIGALRM is ignored and this doesn't play
                                     # nice with some of the (useless - gnulib)
                                     # checks, resulting in hanging builds.
                                     # See http://comments.gmane.org/gmane.comp.lib.gnulib.bugs/13841
                                     # Let's sabotage the tests by stealing
                                     # alarm() from them.
                                     'CFLAGS="$cflags -Dalarm=alarm_disabled"',
                                     'CXXFLAGS="$cxxflags"',
                                     'LDFLAGS="$ldflags"',
                                     'GSED=$sed',
                                     'YACC=$yacc',
                                     '--with-libiconv-prefix=$SDKROOT/usr',
                                     '--disable-static',
                                     '--disable-java',
                                     '--disable-csharp',
                                     '--disable-rpath',
                                     '--disable-dependency-tracking',
                                     '--enable-silent-rules',
                                     '--enable-relocatable',
                                     # Needed for the binaries to work on OS X 10.{7,8}:
                                     '--with-included-libxml',
                                 ],
                                 build_commands=[
                                     # Prevent automake regeneration:
                                     'touch `find . -name aclocal.m4`',
                                     'touch `find . -name configure`',
                                     'touch `find . -name config.h.in`',
                                     'touch `find . -name Makefile.in`',
                                     'touch `find . -name *.1`',
                                     'touch `find . -name *.3`',
                                     'find . -name *.html -exec touch {} \\;',
                                     # Prevent running moopp tool that requires GNU sed and refused to be configured to use gsed:
                                     'touch `find . -name *stream*.[ch]*`',
                                     # Prevent running msgfmt:
                                     'touch `find . -name *.gmo`',
                                 ] + default_build_commands + [
                                     # delete unwanted stuff
                                     'rm -f $destdir/bin/{autopoint,envsubst,gettext*,ngettext,recode-sr-latin}',
                                     # fix dylib references to work
                                     '$top_srcdir/../macos/fixup-dylib-deps.sh //lib @executable_path/../lib $destdir/lib $destdir/bin/*',
                                     # strip executables
                                     'strip -S -u -r $destdir/bin/{msgfmt,msgmerge,msgunfmt,msgcat,xgettext}',
                                     'strip -S -x $destdir/lib/lib*.*.dylib',
                                 ]))

    targets.append(gen_configure(n, 'icu',
                                 srcdir='icu4c',
                                 configure='./source/configure',
                                 flags=[
                                     '--prefix=/',
                                     'CC=$cc',
                                     'CXX=$cxx',
                                     'CFLAGS="$cflags -DU_STATIC_IMPLEMENTATION"',
                                     'CXXFLAGS="$cxxflags -DU_STATIC_IMPLEMENTATION"',
                                     'LDFLAGS="$ldflags"',
                                     '--disable-shared',
                                     '--enable-static',
                                     '--with-data-packaging=archive',
                                     '--disable-tests',
                                     '--disable-samples',
                                 ]))

    n.default(targets)
