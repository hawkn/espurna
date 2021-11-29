import os
import shutil
import tempfile

from .version import app_full_version_for_env


# emulate .ino concatenation to speed up compilation times
def merge_cpp(target, source, env, encoding="utf-8"):
    with tempfile.TemporaryFile() as tmp:
        tmp.write(b"// !!! Automatically generated file; DO NOT EDIT !!! \n")
        tmp.write(
            '#include "{}"\n'.format(
                env.File("${PROJECT_DIR}/espurna/espurna.h").get_abspath()
            ).encode(encoding)
        )
        for src in source:
            src_include = '#include "{}"\n'.format(src.get_abspath())
            tmp.write(src_include.encode(encoding))

        tmp.seek(0)

        with open(target[0].get_abspath(), "wb") as fobj:
            shutil.copyfileobj(tmp, fobj)


def app_add_builder_single_source(env):
    # generate things in the $BUILD_DIR, so there's no need for any extra clean-up code
    source = os.path.join("${BUILD_DIR}", "espurna_single_source", "src", "main.cpp")

    # substitute a single node instead of building it somewhere else as a lib or extra source dir
    # (...and since we can't seem to modify src_filter specifically for the project dir, only middleware works :/)
    def ignore_node(node):
        if node.name.endswith("main.cpp"):
            return env.File(source)
        return None

    project = env.Dir("${PROJECT_DIR}/espurna")
    env.AddBuildMiddleware(ignore_node, os.path.join(project.get_abspath(), "*.cpp"))
    env.Command(source, env.Glob("${PROJECT_DIR}/espurna/*.cpp"), env.Action(merge_cpp))


def firmware_prefix(env):
    return "espurna-{}".format(app_full_version_for_env(env))


# generate an common name for the current build
def firmware_filename(env):
    suffix = "{}.bin".format(env["ESPURNA_BUILD_NAME"] or env["PIOENV"])
    return "-".join([firmware_prefix(env), suffix])


def firmware_destination(env):
    destdir = env["ESPURNA_BUILD_DESTINATION"] or env["PROJECT_DIR"]
    subdir = os.path.join(destdir, firmware_prefix(env))

    return os.path.join(subdir, firmware_filename(env))


def app_add_target_build_and_copy(env):
    from SCons.Script import Copy

    target = firmware_destination(env)

    env.AddTarget(
        "build-and-copy",
        "${BUILD_DIR}/${PROGNAME}.bin",
        env.Command(
            target,
            "${BUILD_DIR}/${PROGNAME}.bin",
            Copy("$TARGET", "$SOURCE")
        ),
        title="Build firmware.bin and store a copy",
        description="Build and store firmware.bin as $ESPURNA_BUILD_DESTINATION/espurna-<version>-$ESPURNA_BUILD_NAME.bin (default destination is $PROJECT_DIR)",
    )
    env.Alias("build-and-copy", target)


# TODO: *could* be a Builder object, just the same it will detect targets via src_suffix & suffix properties
#       but, notice that:
#       - constantly re-generating files in $project_dir only useful in development, as the source tree includes already 'compiled' .re.cpp.inc
#       - .cpp.re.inc <-> .re dependency will be injected into the scons database of the *environment*,
#         which may accidentally cause a rebuild when using 2 envs at the same time
#       - .cpp file `#include`ing the .inc file may be built before the file is actually generated by the re2c (or, in a parallel job)
#         scons has a C source scanner that is tracking `#include` directives. but, file may *already* exist at the scanning phase and
#         it may be assumed something static during the build (as most include directives are for the system, sdk, and the build-tree headers)


def app_add_target_build_re2c(env):
    from SCons.Script import COMMAND_LINE_TARGETS

    targets = [
        env.File(target) for target in COMMAND_LINE_TARGETS if ".re.cpp.inc" in target
    ]
    cmd = "re2c --no-generation-date --case-ranges -W -Werror -o {} {}"
    if targets:
        sources = [
            target.File("{}".format(target.name.replace(".cpp.inc", "")))
            for target in targets
        ]
        for target, source in zip(targets, sources):
            env.Execute(
                env.VerboseAction(
                    cmd.format(target, source), "Generating {}".format(target.name)
                )
            )
        env.Exit(0)
