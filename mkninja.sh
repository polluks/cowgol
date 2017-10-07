#!/bin/sh
set -e

cat <<"EOF"
#############################################################################
###                   THIS FILE IS AUTOGENERATED                          ###
#############################################################################
#
# Don't edit it. Your changes will be destroyed. Instead, edit mkninja.sh
# instead. Next time you run ninja, this file will be automatically updated.

rule mkninja
    command = ./mkninja.sh > $out
    generator = true
build build.ninja : mkninja mkninja.sh

rule bootstrapped_cowgol_program
    command = scripts/cowgol_bootstrap_compiler -o $out $in

rule c_program
    command = cc -std=c99 -Wno-unused-result -O -g -o $out $in

build compiler_suite : phony $
    bin/tokeniser bin/parser bin/typechecker bin/blockifier $
    bin/classifier bin/codegen bin/placer bin/emitter

rule cowgol_program
    command = scripts/cowgol -o $out $in

rule run_smart_test
    command = $in && touch $out

rule run_bbctube_test
    command = scripts/bbctube_test $in $badfile $goodfile && touch $out

rule token_maker
    command = gawk -f src/mk-token-maker.awk $in > $out

rule token_names
    command = gawk -f src/mk-token-names.awk $in > $out

rule mkbbcdist
    command = scripts/mkbbcdist $out
#build bin/bbcdist.zip : mkbbcdist bin/bbc/iopshower bin/bbc/thingshower bin/bbc/tokeniser $
#    src/arch/bbc/lib/runtime.cow src/arch/bbc/lib/mos.cow src/arch/bbc/lib/fileio.cow $
#    src/arch/bbc/lib/argv.cow
EOF

BOOTSTRAP_LIBS="src/arch/bootstrap/host.cow src/utils/names.cow"
BBC_LIBS="src/arch/bbc/host.cow src/arch/bbc/lib/mos.cow src/arch/bbc/lib/runtime.cow src/arch/bbc/lib/fileio.cow src/arch/bbc/lib/argv.cow src/arch/bbc/names.cow"

OBJDIR="/tmp/cowgol-obj"
BOOTSTRAP_DEPENDENCIES="scripts/cowgol_bootstrap_compiler bootstrap/bootstrap.lua bootstrap/cowgol.c bootstrap/cowgol.h $BOOTSTRAP_LIBS"
COWGOL_DEPENDENCIES="scripts/cowgol compiler_suite src/arch/bbc/lib/runtime.cow"
CPUTEST_DEPENDENCIES="scripts/bbctube_test bin/bbctube"

bootstrapped_cowgol_program() {
    out=$1
    shift
    echo "build $out : bootstrapped_cowgol_program $BOOTSTRAP_LIBS $@ | $BOOTSTRAP_DEPENDENCIES"
}

cowgol_program() {
    out=$1
    shift
    echo "build $out : cowgol_program $BBC_LIBS $@ | $COWGOL_DEPENDENCIES"
}

both_cowgol_programs() {
    progname=$1
    shift
    bootstrapped_cowgol_program bin/$progname "$@"
    cowgol_program bin/bbc/$progname "$@"
}

bootstrap_test() {
    src=$1
    basename=${src##*/}
    testname=${basename%.test.cow}
    testbase=$OBJDIR/tests/bootstrap/$testname

    bootstrapped_cowgol_program $testbase \
        tests/bootstrap/_test.cow \
        $src

    echo "build $testbase.stamp : run_smart_test $testbase"
}

cpu_test() {
    src=$1
    basename=${src##*/}
    testname=${basename%.test.cow}
    testbase=$OBJDIR/tests/cpu/$testname

    cowgol_program $testbase.6502.exe \
        $src

    goodfile=tests/cpu/$testname.good
    badfile=tests/cpu/$testname.bad
    echo "build $testbase.stamp : run_bbctube_test $testbase.6502.exe | $goodfile $CPUTEST_DEPENDENCIES"
    echo "  goodfile=$goodfile"
    echo "  badfile=$badfile"
}

c_program() {
    out=$1
    shift
    echo "build $out : c_program $@"
}

both_cowgol_programs tokeniser \
    src/string_lib.cow \
    src/ctype_lib.cow \
    src/numbers_lib.cow \
    src/arch/bbc/globals.cow \
    src/utils/things.cow \
    src/tokeniser/strings.cow \
    src/tokeniser/lexer.cow \
    $OBJDIR/token_names.cow \
    src/tokeniser/tokeniser.cow \
    $OBJDIR/token_maker.cow \
    src/tokeniser/main.cow \

both_cowgol_programs parser \
    src/string_lib.cow \
    src/ctype_lib.cow \
    src/numbers_lib.cow \
    src/arch/bbc/globals.cow \
    src/utils/things.cow \
    $OBJDIR/token_names.cow \
    src/utils/stringtable.cow \
    src/utils/iops.cow \
    src/parser/globals.cow \
    src/parser/symbols.cow \
    src/utils/symbols.cow \
    src/parser/iopwriter.cow \
    src/parser/tokenreader.cow \
    src/parser/constant.cow \
    src/utils/types.cow \
    src/parser/types.cow \
    src/parser/expression.cow \
    src/parser/main.cow

both_cowgol_programs blockifier \
    src/string_lib.cow \
    src/arch/bbc/globals.cow \
    src/utils/things.cow \
    src/utils/iops.cow \
    src/utils/stringtable.cow \
    src/utils/iopreader.cow \
    src/utils/iopwriter.cow \
    src/utils/symbols.cow \
    $OBJDIR/token_names.cow \
    src/utils/types.cow \
    src/blockifier/init.cow \
    src/blockifier/main.cow

bootstrapped_cowgol_program bin/typechecker \
    src/string_lib.cow \
    src/arch/bbc/globals.cow \
    src/utils/things.cow \
    src/utils/iops.cow \
    src/utils/stringtable.cow \
    src/utils/iopreader.cow \
    src/utils/iopwriter.cow \
    src/utils/symbols.cow \
    $OBJDIR/token_names.cow \
    src/utils/types.cow \
    src/typechecker/init.cow \
    src/typechecker/temporaries.cow \
    src/typechecker/tree.cow \
    src/arch/bbc/simplifier.cow \
    src/typechecker/simplifier.cow \
    src/typechecker/main.cow

bootstrapped_cowgol_program bin/classifier \
    src/string_lib.cow \
    src/arch/bbc/globals.cow \
    src/utils/things.cow \
    src/utils/iops.cow \
    src/utils/stringtable.cow \
    src/utils/iopreader.cow \
    src/utils/symbols.cow \
    src/utils/types.cow \
    $OBJDIR/token_names.cow \
    src/classifier/init.cow \
    src/classifier/graph.cow \
    src/arch/bbc/classifier.cow \
    src/classifier/subdata.cow \
    src/classifier/main.cow

bootstrapped_cowgol_program bin/codegen \
    src/string_lib.cow \
    src/arch/bbc/globals.cow \
    src/utils/things.cow \
    src/utils/iops.cow \
    src/utils/stringtable.cow \
    src/utils/iopreader.cow \
    src/utils/iopwriter.cow \
    $OBJDIR/token_names.cow \
    src/utils/symbols.cow \
    src/utils/types.cow \
    src/codegen/init.cow \
    src/codegen/queue.cow \
    src/arch/bbc/codegen0.cow \
    src/arch/bbc/codegen1.cow \
    src/arch/bbc/codegen2_8bit.cow \
    src/arch/bbc/codegen2_wide.cow \
    src/arch/bbc/codegen2_16bit.cow \
    src/arch/bbc/codegen2.cow \
    src/codegen/rules.cow \
    src/codegen/main.cow

both_cowgol_programs placer \
    src/string_lib.cow \
    src/arch/bbc/globals.cow \
    src/utils/things.cow \
    src/utils/iops.cow \
    src/utils/stringtable.cow \
    src/utils/iopreader.cow \
    src/utils/iopwriter.cow \
    src/placer/init.cow \
    src/arch/bbc/placer.cow \
    src/placer/main.cow

both_cowgol_programs emitter \
    src/string_lib.cow \
    src/arch/bbc/globals.cow \
    src/utils/things.cow \
    src/utils/iops.cow \
    src/utils/stringtable.cow \
    src/utils/iopreader.cow \
    src/emitter/init.cow \
    src/arch/bbc/emitter.cow \
    src/emitter/main.cow

both_cowgol_programs thingshower \
    src/string_lib.cow \
    src/arch/bbc/globals.cow \
    src/utils/things.cow \
    src/utils/stringtable.cow \
    src/thingshower/thingshower.cow

both_cowgol_programs iopshower \
    src/string_lib.cow \
    src/arch/bbc/globals.cow \
    src/utils/things.cow \
    src/utils/iops.cow \
    src/utils/stringtable.cow \
    src/iopshower/iopreader.cow \
    src/iopshower/iopshower.cow

c_program bin/bbctube \
    emu/bbctube/bbctube.c \
    emu/bbctube/lib6502.c \

for f in tests/bootstrap/*.test.cow; do
    bootstrap_test $f
done

for f in tests/cpu/*.test.cow; do
    cpu_test $f
done

echo "build $OBJDIR/token_maker.cow : token_maker src/tokens.txt | src/mk-token-maker.awk"
echo "build $OBJDIR/token_names.cow : token_names src/tokens.txt | src/mk-token-names.awk"
